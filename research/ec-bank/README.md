# Eventual-consistency bank with compensating transactions

Research thread: can a transactional system keep a **non-negative-balance
invariant** under *eventual consistency* — letting applications see stale
views and repairing violations automatically — and what does it cost compared
to a strict/opaque STM?  Companion to the book's consistency roadmap
(TODO.md "weaker consistency models in the book").

## The protocol (what we built)

- **Commit = no coordination.** Each transfer becomes an op
  `{id, src, dst, amt}` appended to a local, grow-only, idempotent,
  commutative set (the CRDT) and broadcast asynchronously (reorder,
  duplicates, delays).
- **Views are local folds** over the applied set. A replica may show a
  negative balance while concurrent debits converge — the invariant is
  *eventual*, not enforced at commit time.
- **Automated compensation ("negative transactions").** After each
  delivery, if an account is negative *in this view*, the replica enqueues
  the compensation of the max-id, still-uncompensated transfer debiting that
  account. The culprit rule depends only on the applied set, so all
  detecting replicas emit identical compensations and the views converge to
  the same repaired state. Compensation is a new op (undo-by-doing-the-
  opposite), never an undo: idempotent, commutative, replay-safe.
- **Causal gate.** A compensation is delivered only after its parent op
  (transactional causal consistency, lite).

## What you get / what you pay

| | strict/opaque STM (the book's default) | this EC design |
|---|---|---|
| commit cost | synchronization on every TX | zero coordination |
| observable states | no intermediate/negative values | transient negatives visible |
| isolation | full (serializability/opacity) | none — stale reads, no anti-dependency prevention |
| invariant enforcement | at commit time | after the fact, by compensation |
| external side effects | safe | compensation cannot un-send them |
| convergence | trivially | needs a deterministic culprit rule + causal delivery |

## Artifacts

- `ec_bank_poc.py` — 3-replica threaded PoC: random delivery order,
  duplicate injections, causal gate, max-id culprit compensation.
  Checks at quiescence: convergence (identical op sets), SEC (equal sets ⇒
  equal views), non-negative final balances, money conservation.
  ```sh
  python3 ec_bank_poc.py                    # demo + 800-run stress (all pass)
  python3 ec_bank_poc.py --scenario demo --seed 7
  ```
- `../../docs/proofs/ECBank.tla` + `ECBank.cfg` — finite model
  (2 replicas, 2 transfers oversubscribing one account, causal broadcast).
  TLC exhaustively checks `TypeOK`, `MoneyConserved`, `SEC`,
  `QuiescentSafe` (nothing in flight ⇒ no negative balance anywhere),
  `CausalPending` — 25 distinct states, clean.
  ```sh
  make -C ../../docs/proofs check-ECBank    # or: ./tla.sh check ECBank
  ```
  (Toolchain quirk: the repo's TLC 2.14/SANY rejects `ELSIF`; use
  `ELSE IF`. The TLA+ model avoids a funding op — balances start at
  `BalInit`; the PoC models funding as a `genesis` op exempt from repair.)
- `ec_bank_bench.cpp` — throughput benchmark (C++17, pthreads, one replica
  per thread, in-process async broadcast with periodic merge): measures the
  EC cost of commit (append), fold (reads), and repair (compensations) on
  the book's bank workload. Quiescence asserts SEC / money conservation /
  non-negative finals on every run.
  ```sh
  g++ -O2 -std=c++17 -pthread -o ec_bank_bench ec_bank_bench.cpp
  ./ec_bank_bench -t 4 -a 1024 -n 2000000 -b 5      # full: fold+repair on
  ./ec_bank_bench -t 4 -a 1024 -n 2000000 --commit-only   # append only
  ```
  Measured on a 28-core Xeon E5-2660 v4 (the book's `tab:ec-vs-stm`):
  full EC 0.77 Mops/s (1 replica) → 0.60 (4) — flat in contention;
  append-only path ~17 Mops/s (the fold+repair machinery costs ~22×);
  compensation rate 16–24 % of ops under a tight initial balance
  (`-b 5`), 0 % with slack (`-b 1000`).
  **Communication profile** (same box, 2M ops, after an `alignas(64)` fix
  that removed false sharing between adjacent replicas): the commit path
  is lock-free and writes only thread-private cache lines — one release
  store to the replica's own `published` counter per commit (1.16–1.37
  incl. compensation publishes); gossip reads 1.00 records/commit at one
  replica, 2.27 at two, 3.99–5.12 at four (32–164 B/commit — fan-out
  grows linearly with replicas; merge dedup 0–16 %). Bank state is fully
  replicated: every replica holds all balances, per-account debit sets,
  and the applied op-set (O(A) + O(all-time-ops), the grow-only bill).

## Answer to "is this possible / who invented it"

Yes — with boundaries. What you cannot have is **isolation**: stale reads
and transient invariant violations are observable, and compensation is not
rollback (it cannot recall messages or side effects already taken). The
formal literature maps the trade-off precisely:

- **Invariant confluence** — Bailis, Gupta, Dutta, Friedman, Hellerstein,
  Rehs, Wang, *Coordination Avoidance in Database Systems* (VLDB 2014) and
  *Checking Invariant Confluence* (CIDR 2021): exactly which invariants can
  be maintained without coordination. **Non-negative balances are NOT
  invariant-confluent** → every coordination-free implementation must
  transiently violate them (or reject ops = coordinate). Our design is the
  "violate transiently, repair deterministically" branch.
- **SEC** — Burgader, Guerraoui, Knezevic, Kuznetsov, Pessanha
  (*Understanding Eventual Consistency*, OPODIS 2017): views = folds over
  commutative-idempotent op sets; our applied-set CRDT + fold is textbook
  SEC, with the compensation rule keeping the *reachable* converged states
  safe.
- **Compensating transactions / Sagas** — Garcia-Molina & Salem (1987):
  the "negative transaction" idea, here automated and made convergent.
- **Transactional causal consistency** — Hetz (1993); **Causal+** — Lloyd
  et al. (OSDI 2011); **F1** — Elul, Guerraoui, Kuznetsov (ICDE 2016):
  our causal gate is the minimal slice of these.
- **Adaptive/relaxable guarantees** — Rondit, Cai, Gehrke (ICDE 2015):
  choosing the weakest guarantee that still meets an invariant.
- **CRDTs / Yjs** — Shapiro, Preguiça, Baquero, Zawirski (2011);
  Yjs (<https://github.com/yjs/yjs>) — Yjs's transactions + UndoManager
  show the same "compensation as an ordinary op" trick in a collaborative
  editor.

## Open directions (see TODO.md)

- Prove liveness (every violation eventually repaired) with a fairness
  assumption in TLA+.
- Batch compensation + user-visible "pending repair" state; multiple
  accounts with richer invariants (confluence checker?).
- Contrast in the book with SI's failure modes (bank write-skew demo) —
  same bank example, different point on the consistency/overhead curve.
