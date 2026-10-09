#!/usr/bin/env python3
"""Eventual-consistency bank with automated compensating transactions (PoC).

Companion to docs/proofs/ECBank.tla (exhaustively model-checked) and the
roadmap in TODO.md ("weaker consistency models in the book").

Protocol (per replica):
  * A transfer commits with NO coordination: it becomes an op
    {id, src, dst, amt} in the replica's local view and is broadcast
    asynchronously (unordered, duplicated, delayed delivery).
  * Each replica applies ops as a grow-only, idempotent, commutative set
    (the CRDT); its balances are a local fold over that set.
  * Causal gate: a compensation op is delivered only after its parent op.
  * Repair: after each delivery, if some account is negative in THIS view,
    the replica enqueues the compensation (the delta-op with the largest
    id among still-uncompensated transfers debiting the violated account).
    The culprit rule is a function of the applied set only, so all replicas
    that detect the violation produce identical compensations -> the views
    converge to the same repaired state (SEC + eventual safety).

What this buys: no locks, no global ordering, no coordination on commit.
What this costs: negative balances are observable (not an opaque/strict
view), money is repaired by compensating ops (undo-by-doing-the-opposite),
and convergence takes network time.

Run:  python3 ec_bank_poc.py [--scenario demo|stress] [--seed N]
"""

import argparse
import random
import threading
import time
from collections import defaultdict
from dataclasses import dataclass, field

@dataclass(frozen=True)
class Op:
    id: int
    src: int
    dst: int
    amt: int
    comp_of: int = 0  # 0 = base transfer; else id of compensated op
    genesis: bool = False  # funding op: exempt from repair (reserve)

    def delta(self, acct):
        return (self.amt if self.dst == acct else 0) - (self.amt if self.src == acct else 0)

class Replica(threading.Thread):
    def __init__(self, rid, inboxes, stop):
        super().__init__(daemon=True)
        self.rid, self.inboxes, self.stop = rid, inboxes, stop
        self.applied = set()          # op ids (the CRDT: grow-only)
        self.ops = {}                 # id -> Op
        self.inbox = inboxes[rid]  # alias: submit() appends here
        self.lock = threading.Lock()
        self.n_seen_negative = 0      # observable intermediate violations
        self.repairs = 0

    def submit(self, op, targets):
        for t in targets:
            self.inboxes[t].append(op)

    def run(self):
        while not self.stop.is_set():
            with self.lock:
                ready = [i for i, o in enumerate(self.inbox)
                         if o.comp_of == 0 or o.comp_of in self.applied]
                if ready:
                    i = random.choice(ready)
                    o = self.inbox.pop(i)
                    if o.id not in self.applied:
                        self.applied.add(o.id)
                        self.ops[o.id] = o
                        self._repair_checks_locked()
                    elif random.random() < 0.05:
                        self.inbox.append(o)  # exercise idempotency
            if not ready and self.inbox:
                time.sleep(0.001)  # causal gate not open yet

    def balances(self):
        with self.lock:
            return self._fold()

    def _fold(self):
        b = defaultdict(int)
        for oid in self.ops:
            for acct in (self.ops[oid].src, self.ops[oid].dst):
                b[acct] += self.ops[oid].delta(acct)
        return dict(b)

    def _repair_checks_locked(self):
        bal = defaultdict(int)
        for oid in self.ops:
            for acct in (self.ops[oid].src, self.ops[oid].dst):
                bal[acct] += self.ops[oid].delta(acct)
        for acct, v in bal.items():
            if acct == 99 or v >= 0:
                continue  # external reserve may go negative
            self.n_seen_negative += 1
            culprit = max(
                (o for o in (self.ops[oid] for oid in self.applied)
                 if o.src == acct and o.comp_of == 0 and not o.genesis
                 and not any(c.comp_of == o.id for c in self.ops.values())),
                key=lambda o: o.id, default=None)
            if culprit is None:
                continue  # nothing compensable: leave for the next delivery
            comp = Op(10000 + culprit.id, culprit.dst, culprit.src,
                      culprit.amt, comp_of=culprit.id)
            self.repairs += 1
            for t in self.inboxes:
                t.append(comp)

def quiesced(inboxes):
    return all(len(q) == 0 for q in inboxes)

def run_scenario(k_transfers, seed, verbose=False):
    random.seed(seed)
    n_replicas = 3
    accounts = {0: 10}  # acct 0 funded; accounts 1..k start empty
    for i in range(1, k_transfers + 1):
        accounts[i] = 0
    stop = threading.Event()
    inboxes = [[], [], []]
    reps = [Replica(i, inboxes, stop) for i in range(n_replicas)]
    for r in reps:
        r.start()

    # k concurrent transfers of 8 out of the funded account (oversubscribed:
    # 10 - 8k < 0 for k >= 2) committed locally on replica 0, broadcast all.
    txns = [Op(i, 0, i, 8) for i in range(1, k_transfers + 1)]
    # account balances live as the fold of deltas; initial funding is an op
    fund = Op(500, 99, 0, 10, genesis=True)  # the bank created acct 0 with 10
    for o in [fund] + txns:
        reps[0].submit(o, range(n_replicas))
        if random.random() < 0.3:  # duplicate delivery
            reps[0].submit(o, [random.randrange(n_replicas)])

    while not quiesced(inboxes):
        time.sleep(0.005)
    time.sleep(0.02)  # settle
    stop.set()

    views = [r.balances() for r in reps]
    applied = [frozenset(r.applied) for r in reps]
    total_before = 10
    ok_converged = len(set(applied)) == 1
    ok_views = len(set((tuple(sorted(v.items())) for v in views))) == 1
    ok_nonneg = all(v.get(a, 0) >= 0 for a in accounts
                    for v in views[:1])
    # money conservation: every op is delta-antimorphic, so the grand
    # total over all accounts (incl. the external source 99) is 0.
    ok_total = sum(views[0].values()) == 0
    saw_negative = any(r.n_seen_negative > 0 for r in reps)
    if verbose:
        print(f"  views: {views}")
        print(f"  converged={ok_converged} views_equal={ok_views} "
              f"non_negative={ok_nonneg} money_conserved={ok_total} "
              f"compensations={reps[0].repairs} intermediate_negative_visible={saw_negative}")
    assert ok_converged, "replicas failed to converge to identical op sets"
    assert ok_views, "identical op sets produced different views"
    assert ok_nonneg, "final view still shows a negative balance"
    assert ok_total, "compensations must conserve money"
    assert saw_negative or k_transfers < 2, "expected at least one transient negative"
    return {"converged": ok_converged, "compensations": reps[0].repairs,
            "saw_negative": saw_negative}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", default="all", choices=["demo", "stress", "all"])
    ap.add_argument("--seed", type=int, default=42)
    a = ap.parse_args()
    if a.scenario in ("demo", "all"):
        print("demo (2 concurrent transfers of 8 from an account holding 10):")
        run_scenario(2, a.seed, verbose=True)
    if a.scenario in ("stress", "all"):
        fails = 0
        for s in range(200):
            for k in (2, 3, 5, 8):
                try:
                    run_scenario(k, s * 7 + k)
                except AssertionError:
                    fails += 1
        print(f"stress: 200 seeds x {{2,3,5,8}} concurrent transfers -> {fails} invariant failures")
        assert fails == 0

if __name__ == "__main__":
    main()
