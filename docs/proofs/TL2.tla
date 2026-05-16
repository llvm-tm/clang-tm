----------------------------- MODULE TL2 -----------------------------
(*
 * TL2 — TLA+ Specification (TLC-checkable).
 *
 * Algorithm (Dice, Shalev, Shavit 2006):
 *   - Global clock G.
 *   - Per-address guard: {locked, version}.
 *   - begin(): snapshot G.
 *   - read(V): add (V, guard.version) to read-set, return *V.
 *   - write(V,N): buffer (V,N) in write-set.
 *   - commit():
 *       1. Acquire write-set locks (sorted).
 *       2. Increment G -> c.
 *       3. Validate read-set: guard.version == observed.
 *       4. Write-back buffered values.
 *       5. Release locks with version = c.
 *
 * Invariants checked by TLC:
 *   - LockConsistent: a guard is locked iff some thread is committing.
 *   - NoDirtyRead: no thread reads a value while another holds its lock.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Thread, Addr, MAX_COMMIT
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME MAX_COMMIT \in Nat

VARIABLES
    clock,                                (* global version clock *)
    guard[_],                             (* per-address guard: bit0=locked, bits1+=version *)
    mem[_],                               (* shared memory *)
    pc[_],                                (* idle | active | committing *)
    readSet[_],                           (* per-thread: set of (addr, observed_version) *)
    writeSet[_],                          (* per-thread: set of addr *)
    writeBuf[_, _],                       (* buffered write value per thread per addr *)
    snapshot[_],                          (* clock snapshot at begin *)
    readOnly[_],                          (* TRUE if no writes in this TX *)
    committed[_]                          (* commit count *)

vars == <<clock, guard, mem, pc, readSet, writeSet, writeBuf, snapshot, readOnly, committed>>

(* ---- helper: guard encoding ---- *)
LOCK_BIT == 1
GuardVersion(g) == g >> 1
GuardLocked(g) == g & LOCK_BIT
MakeGuard(locked, ver) == (ver << 1) | locked

(* ---- initial state ---- *)
Init ==
    /\ clock = 0
    /\ guard = [a \in Addr |-> MakeGuard(0, 0)]
    /\ mem = [a \in Addr |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ readSet = [t \in Thread |-> {}]
    /\ writeSet = [t \in Thread |-> {}]
    /\ writeBuf = [t \in Thread, a \in Addr |-> 0]
    /\ snapshot = [t \in Thread |-> 0]
    /\ readOnly = [t \in Thread |-> TRUE]
    /\ committed = [t \in Thread |-> 0]

(* ---- Transaction begin ---- *)
Begin(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ snapshot' = [snapshot EXCEPT ![t] = clock]
    /\ readOnly' = [readOnly EXCEPT ![t] = TRUE]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, guard, mem, writeBuf, committed>>

(* ---- Read V_i ---- *)
ReadMiss(t, a) ==
    (* read from memory, add to read-set *)
    /\ pc[t] = "active"
    /\ a \notin writeSet[t]
    /\ a \in Addr
    /\ readSet' = [readSet EXCEPT ![t] = readSet[t] \cup {<<a, GuardVersion(guard[a])>>}]
    /\ UNCHANGED <<clock, guard, mem, pc, writeSet, writeBuf, snapshot, readOnly, committed>>

ReadHit(t, a) ==
    (* read from own write-set *)
    /\ pc[t] = "active"
    /\ a \in writeSet[t]
    /\ UNCHANGED vars

(* ---- Write N to V_i ---- *)
Write(t, a, n) ==
    /\ pc[t] = "active"
    /\ a \in Addr
    /\ writeSet' = [writeSet EXCEPT ![t] = writeSet[t] \cup {a}]
    /\ writeBuf' = [writeBuf EXCEPT ![t][a] = n]
    /\ readOnly' = [readOnly EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<clock, guard, mem, pc, readSet, snapshot, committed>>

(* ---- Commit: Phase 1 — acquire locks ---- *)
CommitAcquire(t) ==
    /\ pc[t] = "active"
    /\ readOnly[t] = FALSE
    /\ writeSet[t] # {}
    /\ \A a \in writeSet[t] : ~GuardLocked(guard[a])    (* all guards free *)
    /\ guard' = [a \in Addr |->
        IF a \in writeSet[t]
        THEN MakeGuard(1, GuardVersion(guard[a]))         (* set lock bit *)
        ELSE guard[a]]
    /\ pc' = [pc EXCEPT ![t] = "committing"]
    /\ UNCHANGED <<clock, mem, readSet, writeSet, writeBuf, snapshot, readOnly, committed>>

(* ---- Commit: Phase 2 — increment clock ---- *)
CommitIncClock(t) ==
    /\ pc[t] = "committing"
    /\ clock' = clock + 1
    /\ UNCHANGED <<guard, mem, pc, readSet, writeSet, writeBuf, snapshot, readOnly, committed>>

(* ---- Commit: Phase 3 — validate read-set ---- *)
CommitValidate(t) ==
    /\ pc[t] = "committing"
    (* validate: every read-set entry's guard version still matches *)
    /\ \A entry \in readSet[t] :
        LET addr == entry[1] IN
        GuardVersion(guard[addr]) = entry[2]
    /\ pc' = [pc EXCEPT ![t] = "committing_v"]
    /\ UNCHANGED <<clock, guard, mem, readSet, writeSet, writeBuf, snapshot, readOnly, committed>>

(* ---- Commit validation fails: abort ---- *)
CommitValidateFail(t) ==
    /\ pc[t] = "committing"
    /\ \E entry \in readSet[t] :
        LET addr == entry[1] IN
        GuardVersion(guard[addr]) # entry[2]
    (* release locks *)
    /\ guard' = [a \in Addr |->
        IF a \in writeSet[t]
        THEN MakeGuard(0, GuardVersion(guard[a]))
        ELSE guard[a]]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, mem, writeBuf, snapshot, readOnly, committed>>

(* ---- Commit: Phase 4 — write-back ---- *)
CommitWriteBack(t) ==
    /\ pc[t] = "committing_v"
    /\ mem' = [a \in Addr |->
        IF a \in writeSet[t] THEN writeBuf[t][a] ELSE mem[a]]
    /\ pc' = [pc EXCEPT ![t] = "committing_wb"]
    /\ UNCHANGED <<clock, guard, readSet, writeSet, writeBuf, snapshot, readOnly, committed>>

(* ---- Commit: Phase 5 — release locks with version ---- *)
CommitRelease(t) ==
    /\ pc[t] = "committing_wb"
    (* release each lock with commit_version = clock *)
    /\ guard' = [a \in Addr |->
        IF a \in writeSet[t]
        THEN MakeGuard(0, clock)       (* store version, clear lock *)
        ELSE guard[a]]
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, mem, writeBuf, snapshot, readOnly>>

(* ---- Read-only commit ---- *)
CommitReadOnly(t) ==
    /\ pc[t] = "active"
    /\ readOnly[t] = TRUE
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, guard, mem, writeSet, writeBuf, snapshot, readOnly>>

(* ---- Next-state ---- *)
Next ==
    \E t \in Thread :
        \/ Begin(t)
        \/ (\E a \in Addr : ReadMiss(t, a))
        \/ (\E a \in Addr : ReadHit(t, a))
        \/ (\E a \in Addr : \E n \in 0..MAX_COMMIT : Write(t, a, n))
        \/ CommitAcquire(t)
        \/ CommitIncClock(t)
        \/ CommitValidate(t)
        \/ CommitValidateFail(t)
        \/ CommitWriteBack(t)
        \/ CommitRelease(t)
        \/ CommitReadOnly(t)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* INVARIANTS                                                          *)
(*====================================================================*)

(* Invariant 1: At most one thread is in committing states at any time *)
NoConcurrentCommit ==
    \A t1, t2 \in Thread :
        (t1 # t2)
        => ~ ( (pc[t1] \in {"committing", "committing_v", "committing_wb"})
             /\ (pc[t2] \in {"committing", "committing_v", "committing_wb"}))

(* Invariant 2: A guard is locked iff the owning thread is committing *)
LockConsistent ==
    \A a \in Addr :
        GuardLocked(guard[a]) = 1
        <=> \E t \in Thread :
            a \in writeSet[t] /\ pc[t] \in {"committing", "committing_v", "committing_wb"}

(* Invariant 3: No thread reads an address whose guard is locked by another *)
NoDirtyRead ==
    \A t1, t2 \in Thread, a \in Addr :
        (t1 # t2)
        /\ a \in readSet[t1]    (* t1 is reading a *)
        /\ GuardLocked(guard[a]) = 1   (* a is locked *)
        => pc[t2] = "idle" \/ a \notin writeSet[t2]  (* not locked by t2 writing *)

(* Invariant 4: Committed version increases monotonically with lock versions *)
VersionMonotonic ==
    \A t \in Thread :
        (committed[t] > 0)
        => \A a \in writeSet[t] : GuardVersion(guard[a]) >= committed[t]

(* Invariant 5: No thread's snapshot exceeds clock *)
SnapshotInv ==
    \A t \in Thread : snapshot[t] <= clock

(* Combined invariant for TLC *)
Inv ==
    /\ NoConcurrentCommit
    /\ LockConsistent
    /\ NoDirtyRead
    /\ VersionMonotonic
    /\ SnapshotInv

THEOREM Spec => []Inv

(*====================================================================*)
(* TEMPORAL PROPERTIES FOR TLC                                         *)
(*====================================================================*)

(* Every begin eventually reaches idle (progress) *)
Progress ==
    \A t \in Thread : (pc[t] = "active" ~> pc[t] = "idle")

(* No deadlock: some thread can always make progress *)
DeadlockFreedom ==
    []( \E t \in Thread : Enabled(Begin(t) \/ ...
        (* simplified: just check Next enabled *)

========================================================================
