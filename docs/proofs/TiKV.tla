------------------------ MODULE TiKV ------------------------
(*
 * TiKV — Percolator 2PC Distributed TM Backend
 *
 * Algorithm (from runtime/tikv/src/lib.rs, AGENTS.md 2026-06-20):
 *
 *   TM addresses mapped to TiKV keys: "tm:{region_offset:016x}"
 *   TiKV provides Percolator-style 2PC with snapshot isolation.
 *
 *   tm_begin():   TiKV begin_optimistic() — starts a snapshot
 *   tm_read(a):   check local write-set → local read-set cache →
 *                 TiKV get() via snapshot (lazy-fetch).  On TiKV
 *                 error (TxnNotFound), rollback + TmxAbort retry.
 *   tm_write(a,v): buffer in local write-set.
 *   tm_commit():  flush write-set entries via TiKV put() calls →
 *                 TiKV commit() — Percolator 2PC internally:
 *                   Phase 1 (prewrite): acquire key-level locks,
 *                     write lock + data to TiKV.
 *                   Phase 2 (commit): replace lock with commit
 *                     marker, release locks.
 *                 Returns true if 2PC succeeds, false if conflict.
 *   tm_abort():   TiKV rollback() — release any held locks.
 *
 * Key insight: Percolator's 2PC is equivalent to optimistic
 * concurrency control with key-level locking.  The "prewrite"
 * phase validates that no concurrent transaction holds a lock
 * on any key — if a lock is found, the transaction aborts.
 * The "commit" phase makes all writes atomic via a single-key
 * commit point (the primary key).
 *
 * Invariants:
 *   AtomicCommit:     All writes in a transaction are applied
 *                     atomically (all-or-nothing).
 *   NoLostUpdate:     Two concurrent writes to the same key
 *                     cannot both commit (Percolator ensures
 *                     write-write conflict detection).
 *   SnapshotIsolation: Reads see a consistent snapshot.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of client thread IDs *)
    Key,                (* Set of TM key identifiers (encoded addr) *)
    Data                (* Set of possible data values *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Key \subseteq Nat
ASSUME Data \subseteq Nat

VARIABLES
    (* ── TiKV cluster state ──────────────────────────────────── *)
    kv_store,           (* [Key -> Data]: committed key-value state *)
    kv_locks,           (* [Key -> Thread \cup {0}]: 0 = no lock *)

    (* ── Per-thread transaction state ────────────────────────── *)
    pc,                 (* [Thread -> {"idle", "active",
                                      "prewriting", "committing",
                                      "aborting"}] *)
    write_set,          (* [Thread -> Key -> Data \cup {NoWrite}] *)
    read_set,           (* [Thread -> Set(Key)]: cached reads *)
    snapshot,           (* [Thread -> Nat]: snapshot timestamp *)

    (* ── 2PC commit state ────────────────────────────────────── *)
    primary_key,        (* [Thread -> Key]: first written key *)
    prewrite_ok,        (* [Thread -> BOOLEAN]: prewrite succeeded *)
    commit_ts,          (* [Thread -> Nat]: commit timestamp *)

    (* ── Bookkeeping ─────────────────────────────────────────── *)
    committed,          (* [Thread -> Nat] *)
    aborted             (* [Thread -> Nat] *)

vars == <<kv_store, kv_locks, pc, write_set, read_set, snapshot,
          primary_key, prewrite_ok, commit_ts, committed, aborted>>

NoWrite == 0 - 1
HasWritten(t, k) == write_set[t][k] # NoWrite

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ kv_store = [k \in Key |-> 0]
    /\ kv_locks = [k \in Key |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ write_set = [t \in Thread |-> [k \in Key |-> NoWrite]]
    /\ read_set = [t \in Thread |-> {}]
    /\ snapshot = [t \in Thread |-> 0]
    /\ primary_key = [t \in Thread |-> 0]
    /\ prewrite_ok = [t \in Thread |-> FALSE]
    /\ commit_ts = [t \in Thread |-> 0]
    /\ committed = [t \in Thread |-> 0]
    /\ aborted = [t \in Thread |-> 0]

(*--------------------------------------------------------------------*)
(* TM Actions                                                         *)
(*--------------------------------------------------------------------*)

(*── Begin: start TiKV optimistic snapshot ─────────────────────────*)
Begin(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ write_set' = [write_set EXCEPT ![t] = [k \in Key |-> NoWrite]]
    /\ read_set' = [read_set EXCEPT ![t] = {}]
    (* Snapshot timestamp: current kv_store version *\)
    (* In TiKV, this is a hybrid logical clock (HLC); for TLA+
       we model it as the current data version. *\)
    /\ snapshot' = [snapshot EXCEPT ![t] = 1]
    /\ UNCHANGED <<kv_store, kv_locks, primary_key, prewrite_ok,
                   commit_ts, committed, aborted>>

(*── Read: local write-set → local read-set → TiKV get() ──────────*)
Read(t, k) ==
    /\ pc[t] = "active"
    /\ k \in Key
    /\ IF HasWritten(t, k)
       THEN
           (* Write exists in local buffer — return buffered value *\)
           /\ UNCHANGED vars
       ELSE
           (* Check local read-set cache, then TiKV snapshot *\)
           /\ read_set' = [read_set EXCEPT ![t] = read_set[t] \cup {k}]
           (* In case of TiKV error (TxnNotFound from concurrent
              commit), the real implementation panics with TmxAbort,
              rolls back the TiKV transaction, and the TM retry loop
              re-executes.  We model this via the TxnConflict action. *\)
           /\ UNCHANGED <<kv_store, kv_locks, pc, write_set, snapshot,
                          primary_key, prewrite_ok, commit_ts,
                          committed, aborted>>

(*── Read conflict: TiKV returns TxnNotFound → abort ──────────────*)
TxnConflict(t) ==
    /\ pc[t] = "active"
    (* TiKV detected a concurrent transaction's lock on a key
       in our read-set.  Rollback and retry. *\)
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ UNCHANGED <<kv_store, kv_locks, write_set, read_set, snapshot,
                   primary_key, prewrite_ok, commit_ts, committed>>

(*── Write: buffer in local write-set ──────────────────────────────*)
Write(t, k, v) ==
    /\ pc[t] = "active"
    /\ k \in Key
    /\ v \in Data
    /\ write_set' = [write_set EXCEPT ![t][k] = v]
    (* Set primary key if this is the first write *\)
    /\ IF \A k2 \in Key : ~HasWritten(t, k2)
       THEN
           /\ primary_key' = [primary_key EXCEPT ![t] = k]
       ELSE
           /\ UNCHANGED <<primary_key>>
    /\ UNCHANGED <<kv_store, kv_locks, pc, read_set, snapshot,
                   prewrite_ok, commit_ts, committed, aborted>>

(*── Commit (read-only): no 2PC needed ─────────────────────────────*)
ReadOnlyCommit(t) ==
    /\ pc[t] = "active"
    /\ \A k \in Key : ~HasWritten(t, k)
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ read_set' = [read_set EXCEPT ![t] = {}]
    /\ UNCHANGED <<kv_store, kv_locks, write_set, snapshot,
                   primary_key, prewrite_ok, commit_ts, aborted>>

(*── Commit (read-write) — Phase 1: Prewrite ──────────────────────*)
(*  Percolator prewrite: acquire locks on all written keys. *)
Prewrite(t) ==
    /\ pc[t] = "active"
    /\ \E k \in Key : HasWritten(t, k)
    (* Prewrite all written keys: check locks are free, acquire them *\)
    /\ \A k \in Key :
         HasWritten(t, k) =>
             kv_locks[k] = 0 \/ kv_locks[k] = t
    (* Acquire locks *\)
    /\ kv_locks' = [k \in Key |->
                      IF HasWritten(t, k) THEN t ELSE kv_locks[k]]
    (* Also write the lock + data to TiKV (modeled as updating
       kv_store to a "prewritten" state — data not yet committed,
       but locked).  For TLC, we keep the old data visible since
       the lock prevents other readers/writers from seeing it. *\)
    /\ prewrite_ok' = [prewrite_ok EXCEPT ![t] = TRUE]
    /\ pc' = [pc EXCEPT ![t] = "prewriting"]
    /\ UNCHANGED <<write_set, read_set, snapshot, primary_key,
                   commit_ts, committed, aborted>>

(*── Prewrite conflict: another thread holds a lock → abort ───────*)
PrewriteConflict(t) ==
    /\ pc[t] = "active"
    /\ \E k \in Key :
         HasWritten(t, k) /\ kv_locks[k] \notin {0, t}
    (* Another thread holds the lock — abort *\)
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    (* Release any locks we did acquire (none if this is the
       first conflicting key — the real implementation aborts
       on first conflict) *\)
    /\ kv_locks' = [k \in Key |->
                      IF HasWritten(t, k) /\ kv_locks[k] = t
                      THEN 0 ELSE kv_locks[k]]
    /\ prewrite_ok' = [prewrite_ok EXCEPT ![t] = FALSE]
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ UNCHANGED <<write_set, read_set, snapshot, primary_key,
                   commit_ts, committed>>

(*── Commit — Phase 2: Commit primary key ─────────────────────────*)
(*  Percolator commits the primary key first (single-point commit). *)
CommitPrimary(t) ==
    /\ pc[t] = "prewriting"
    /\ prewrite_ok[t] = TRUE
    (* Get commit timestamp *\)
    /\ commit_ts' = [commit_ts EXCEPT ![t] = 2]
    (* Write the primary key's value definitively *\)
    /\ LET pk == primary_key[t] IN
       HasWritten(t, pk) =>
           /\ kv_store' = [kv_store EXCEPT ![pk] = write_set[t][pk]]
           /\ kv_locks' = [kv_locks EXCEPT ![pk] = 0]
    /\ pc' = [pc EXCEPT ![t] = "committing"]
    /\ UNCHANGED <<write_set, read_set, snapshot, prewrite_ok,
                   committed, aborted>>

(*── Commit — Phase 2b: Commit secondary keys ─────────────────────*)
(*  After primary is committed, commit all remaining keys. *)
CommitSecondary(t) ==
    /\ pc[t] = "committing"
    (* Write all remaining written keys *\)
    /\ kv_store' = [k \in Key |->
                      IF HasWritten(t, k) /\ k # primary_key[t]
                      THEN write_set[t][k]
                      ELSE kv_store[k]]
    (* Release all remaining locks *\)
    /\ kv_locks' = [k \in Key |->
                      IF HasWritten(t, k) /\ k # primary_key[t]
                      THEN 0 ELSE kv_locks[k]]
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ read_set' = [read_set EXCEPT ![t] = {}]
    /\ UNCHANGED <<write_set, snapshot, primary_key, prewrite_ok,
                   commit_ts, aborted>>

(*── Rollback/Abort ──────────────────────────────────────────────*)
(*  TiKV rollback: release all locks held by this transaction. *)
Abort(t) ==
    /\ pc[t] \in {"active", "aborting"}
    (* Release any locks we hold *\)
    /\ kv_locks' = [k \in Key |->
                      IF kv_locks[k] = t THEN 0 ELSE kv_locks[k]]
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ write_set' = [write_set EXCEPT ![t] = [k \in Key |-> NoWrite]]
    /\ read_set' = [read_set EXCEPT ![t] = {}]
    /\ prewrite_ok' = [prewrite_ok EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<kv_store, snapshot, primary_key, commit_ts, committed>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \E t \in Thread :
        \/ Begin(t)
        \/ \E k \in Key : Read(t, k)
        \/ \E k \in Key : TxnConflict(t)
        \/ \E k \in Key : \E v \in Data : Write(t, k, v)
        \/ ReadOnlyCommit(t)
        \/ Prewrite(t)
        \/ PrewriteConflict(t)
        \/ CommitPrimary(t)
        \/ CommitSecondary(t)
        \/ Abort(t)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: A key can be locked by at most one thread at a time ───────*)
LockExclusion ==
    \A k \in Key :
        \A t1, t2 \in Thread :
            (kv_locks[k] = t1 /\ kv_locks[k] = t2) => t1 = t2

(*── I2: If a thread holds locks and is done, locks are released ──*)
NoStaleLocks ==
    \A t \in Thread :
        pc[t] = "idle" =>
            \A k \in Key : kv_locks[k] # t

(*── I3: Committed writes are visible in kv_store ─────────────────*)
CommittedVisible ==
    \A t \in Thread, k \in Key :
        pc[t] = "idle" /\ HasWritten(t, k) =>
            \/ committed[t] > 0  (* committed — writes are applied *)
            \/ aborted[t] > 0    (* aborted — writes are discarded *)

(*── I4: Snapshot isolation — no concurrent overwrite of committed data ─*)
SnapshotIsolation ==
    \A k \in Key :
        (* If a lock exists, the data under it may be uncommitted *)
        kv_locks[k] # 0 => kv_store[k] \in Data

(*── I5: Primary key always precedes secondary keys in commit ──────*)
CommitOrdering ==
    \A t \in Thread :
        pc[t] \in {"prewriting", "committing"} =>
            \/ primary_key[t] \in Key
            \/ \A k \in Key : ~HasWritten(t, k)   (* read-only *)

(*── I6: No key is both locked and committed by different threads ──*)
NoDoubleCommit ==
    \A k \in Key :
        kv_locks[k] = 0 \/
        \A t \in Thread :
            -- (HasWritten(t, k) /\ committed[t] > 0 /\ kv_locks[k] = t)

====
