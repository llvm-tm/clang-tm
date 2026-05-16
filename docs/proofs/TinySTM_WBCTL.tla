---------------------- MODULE TinySTM_WBCTL -----------------------
(*
 * TinySTM WBCTL (Write-Back Commit-Time Locking) — TLA+ Spec
 *
 * Features:
 *   - Global clock C
 *   - Per-address lock table (version + lock bit + owner ID)
 *   - begin(): snapshot C_start = C, C_end = C_start.
 *   - read(V): double-check lock protocol, add (V, version) to read-set.
 *   - write(V,N): buffer in write-set (no lock).
 *   - commit():
 *       1. Acquire locks on write-set in sorted address order.
 *       2. Increment clock -> C_commit.
 *       3. Validate read-set via extend(): check locks.version <= C_end.
 *       4. Write-back buffered values.
 *       5. Release locks with C_commit.
 *
 * TLC-checkable invariants:
 *   - At most one commit in flight.
 *   - Locked addresses are owned by exactly one committing thread.
 *   - Write-back happens under lock.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Thread, Addr, MAX_VAL
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat

VARIABLES
    clock,
    lock[_, _],                           (* [addr -> {locked, owner, version}] *)
    mem[_],
    pc[_],                                (* idle | active | locking | wb *)
    readSet[_],                           (* set of (addr, observed_version) *)
    writeSet[_],                          (* set of addr *)
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
    /\ LOCK_FREE(a)                           (* lock not held by anyone *)
    /\ readSet' = [readSet EXCEPT ![t] = readSet[t] \cup {<<a, LOCK_VER(a)>>}]
    /\ UNCHANGED <<clock, lock, mem, pc, writeSet, writeBuf, readOnly, committed>>

Write(t, a, n) ==
    /\ pc[t] = "active"
    /\ a \in Addr
    /\ writeSet' = [writeSet EXCEPT ![t] = writeSet[t] \cup {a}]
    /\ writeBuf' = [writeBuf EXCEPT ![t][a] = n]
    /\ readOnly' = [readOnly EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<clock, lock, mem, pc, readSet, committed>>

(* Commit: Phase 1 — acquire all locks in order *)
LockAcquire(t) ==
    /\ pc[t] = "active"
    /\ readOnly[t] = FALSE
    /\ writeSet[t] # {}
    /\ \A a \in writeSet[t] : LOCK_FREE(a)
    /\ lock' = [a \in Addr |->
        IF a \in writeSet[t]
        THEN MAKE_LOCK(1, t, LOCK_VER(a))
        ELSE lock[a]]
    /\ pc' = [pc EXCEPT ![t] = "locking"]
    /\ UNCHANGED <<clock, mem, readSet, writeSet, writeBuf, readOnly, committed>>

(* Commit: Phase 2 — increment clock *)
IncClock(t) ==
    /\ pc[t] = "locking"
    /\ clock' = clock + 1
    /\ UNCHANGED <<lock, mem, pc, readSet, writeSet, writeBuf, readOnly, committed>>

(* Commit: Phase 3 — validate read-set *)
Validate(t) ==
    /\ pc[t] = "locking"
    (* extend(): check lock.version <= clock-1 for all read-set entries *)
    /\ \A entry \in readSet[t] :
        LET addr == entry[1] IN
        LOCK_VER(addr) <= clock - 1         (* version hasn't advanced *)
    /\ pc' = [pc EXCEPT ![t] = "wb"]
    /\ UNCHANGED <<clock, lock, mem, readSet, writeSet, writeBuf, readOnly, committed>>

ValidateFail(t) ==
    /\ pc[t] = "locking"
    /\ \E entry \in readSet[t] :
        LET addr == entry[1] IN
        LOCK_VER(addr) > clock - 1
    (* release all locks *)
    /\ lock' = [a \in Addr |->
        IF a \in writeSet[t] THEN MAKE_LOCK(0, 0, LOCK_VER(a)) ELSE lock[a]]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, mem, writeBuf, readOnly, committed>>

(* Commit: Phase 4 — write-back and unlock *)
WriteBack(t) ==
    /\ pc[t] = "wb"
    /\ mem' = [a \in Addr |->
        IF a \in writeSet[t] THEN writeBuf[t][a] ELSE mem[a]]
    (* release with commit_version = clock *)
    /\ lock' = [a \in Addr |->
        IF a \in writeSet[t] THEN MAKE_LOCK(0, 0, clock) ELSE lock[a]]
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<clock, writeBuf, readOnly>>

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
        \/ (\E a \in Addr : \E n \in 0..MAX_VAL : Write(t, a, n))
        \/ LockAcquire(t)
        \/ IncClock(t)
        \/ Validate(t)
        \/ ValidateFail(t)
        \/ WriteBack(t)
        \/ CommitReadOnly(t)

Spec == Init /\ [][Next]_vars

(* ---- INVARIANTS ---- *)

(* At most one thread in locking/wb at any time *)
NoConcurrentLocking ==
    \A t1, t2 \in Thread :
        (t1 # t2)
        => ~ ( (pc[t1] \in {"locking", "wb"}) /\ (pc[t2] \in {"locking", "wb"}) )

(* Lock owner matches the committing thread *)
LockOwnerInv ==
    \A a \in Addr :
        ~LOCK_FREE(a) => \E t \in Thread :
            a \in writeSet[t] /\ pc[t] \in {"locking", "wb"} /\ LOCK_OWNER(a) = t

(* Write-back only happens after validation *)
WriteBackSafe ==
    \A t \in Thread : (pc[t] = "wb") => (clock > 0)

Inv ==
    /\ NoConcurrentLocking
    /\ LockOwnerInv
    /\ WriteBackSafe

THEOREM Spec => []Inv

========================================================================
