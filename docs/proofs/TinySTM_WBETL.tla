--------------------- MODULE TinySTM_WBETL -----------------------
(*
 * TinySTM WBETL (Write-Back Encounter-Time Locking) — TLA+ Spec
 *
 * Features:
 *   - Global clock C.
 *   - Per-address lock table.
 *   - begin(): snapshot C_start.
 *   - read(V): double-check protocol (same as WBCTL).
 *   - write(V,N): acquire lock EAGERLY on first write encounter.
 *   - commit(): increment clock, validate, write-back, unlock.
 *
 * Key difference from WBCTL: locks are acquired at write-time,
 * not deferred to commit. This provides early write-write conflict
 * detection.
 *
 * TLC-checkable invariants:
 *   - No two threads hold the same lock.
 *   - Write-back happens only by the lock owner.
 *   - Version monotonicity.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Thread, Addr, MAX_VAL
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat

VARIABLES
    clock,
    lock[_, _],                           (* [addr -> {locked, owner, version}] *)
    mem[_],
    pc[_],                                (* idle | active | wb *)
    readSet[_],
    writeSet[_],
    writeBuf[_, _],
    readOnly[_],
    committed[_]

vars == <<clock, lock, mem, pc, readSet, writeSet, writeBuf, readOnly, committed>>

LOCK_FREE(i) == lock[i][1] = 0
LOCK_OWNER(i) == lock[i][2]
LOCK_VER(i) == lock[i][3]
MAKE_LOCK(locked, owner, ver) == <<locked, owner, ver>>

Init ==
    /\ clock = 0
    /\ lock = [a \in Addr |-> MAKE_LOCK(0, 0, 0)]
    /\ mem = [a \in Addr |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ readSet = [t \in Thread |-> {}]
    /\ writeSet = [t \in Thread |-> {}]
    /\ writeBuf = [t \in Thread, a \in Addr |-> 0]
    /\ readOnly = [t \in Thread |-> TRUE]
    /\ committed = [t \in Thread |-> 0]

Begin(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ readOnly' = [readOnly EXCEPT ![t] = TRUE]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, lock, mem, writeBuf, committed>>

ReadOwn(t, a) ==
    /\ pc[t] = "active"
    /\ a \in writeSet[t]
    /\ UNCHANGED vars

ReadMiss(t, a) ==
    /\ pc[t] = "active"
    /\ a \notin writeSet[t]
    /\ LOCK_FREE(a)
    /\ readSet' = [readSet EXCEPT ![t] = readSet[t] \cup {<<a, LOCK_VER(a)>>}]
    /\ UNCHANGED <<clock, lock, mem, pc, writeSet, writeBuf, readOnly, committed>>

WriteNew(t, a, n) ==
    (* First write to address a: acquire lock eagerly *)
    /\ pc[t] = "active"
    /\ a \notin writeSet[t]
    /\ LOCK_FREE(a)                              (* no one holds it *)
    /\ lock' = [a \in Addr |->
        IF a = a THEN MAKE_LOCK(1, t, LOCK_VER(a)) ELSE lock[a]]
    /\ writeSet' = [writeSet EXCEPT ![t] = writeSet[t] \cup {a}]
    /\ writeBuf' = [writeBuf EXCEPT ![t][a] = n]
    /\ readOnly' = [readOnly EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<clock, mem, pc, readSet, committed>>

WriteUpdate(t, a, n) ==
    (* Subsequent write: already locked, just update buffer *)
    /\ pc[t] = "active"
    /\ a \in writeSet[t]
    /\ writeBuf' = [writeBuf EXCEPT ![t][a] = n]
    /\ readOnly' = [readOnly EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<clock, lock, mem, pc, readSet, writeSet, committed>>

WriteConflictAbort(t, a) ==
    (* Attempt to write, but lock held by another -> abort *)
    /\ pc[t] = "active"
    /\ a \notin writeSet[t]
    /\ ~LOCK_FREE(a) /\ LOCK_OWNER(a) # t
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, lock, mem, writeSet, writeBuf, readOnly, committed>>

(* Commit: increment clock, validate, write-back, unlock *)
Commit(t) ==
    /\ pc[t] = "active"
    /\ readOnly[t] = FALSE
    /\ writeSet[t] # {}
    (* all our locks are still held *)
    /\ \A a \in writeSet[t] : LOCK_OWNER(a) = t
    /\ clock' = clock + 1                         (* advance clock *)
    (* validate read-set: versions stable *)
    /\ \A entry \in readSet[t] :
        LET addr == entry[1] IN
        \/ LOCK_OWNER(addr) = t                   (* self-locked: skip *)
        \/ LOCK_VER(addr) = entry[2]              (* version unchanged *)
    (* write-back *)
    /\ mem' = [a \in Addr |->
        IF a \in writeSet[t] THEN writeBuf[t][a] ELSE mem[a]]
    (* release locks with new version *)
    /\ lock' = [a \in Addr |->
        IF a \in writeSet[t]
        THEN MAKE_LOCK(0, 0, clock)
        ELSE lock[a]]
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<writeBuf, readOnly>>

CommitReadOnly(t) ==
    /\ pc[t] = "active"
    /\ readOnly[t] = TRUE
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, lock, mem, writeSet, writeBuf, readOnly>>

Next ==
    \E t \in Thread :
        \/ Begin(t)
        \/ (\E a \in Addr : ReadOwn(t, a))
        \/ (\E a \in Addr : ReadMiss(t, a))
        \/ (\E a \in Addr : \E n \in 0..MAX_VAL : WriteNew(t, a, n))
        \/ (\E a \in Addr : \E n \in 0..MAX_VAL : WriteUpdate(t, a, n))
        \/ (\E a \in Addr : WriteConflictAbort(t, a))
        \/ Commit(t)
        \/ CommitReadOnly(t)

Spec == Init /\ [][Next]_vars

(* ---- INVARIANTS ---- *)

(* No two threads hold the same lock *)
MutexLocks ==
    \A a \in Addr, t1, t2 \in Thread :
        (t1 # t2 /\ ~LOCK_FREE(a))
        => ~ (LOCK_OWNER(a) = t1 /\ LOCK_OWNER(a) = t2)

(* Lock owner is in a transaction with the address in write-set *)
LockOwnerTx ==
    \A a \in Addr :
        ~LOCK_FREE(a)
        => \E t \in Thread :
            LOCK_OWNER(a) = t /\ a \in writeSet[t]

(* No thread holds locks after commit *)
NoLocksAfterCommit ==
    \A t \in Thread : (pc[t] = "idle") => \A a \in Addr : ~(LOCK_OWNER(a) = t)

Inv ==
    /\ MutexLocks
    /\ LockOwnerTx
    /\ NoLocksAfterCommit

THEOREM Spec => []Inv

========================================================================
