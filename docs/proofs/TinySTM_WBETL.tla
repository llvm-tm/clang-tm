--------------------- MODULE TinySTM_WBETL ------------------------
(*
 * TinySTM WBETL (Write-Back Encounter-Time Locking) — TLA+ Spec (PlusCal)
 *
 * Features:
 *   - Global clock C.
 *   - Per-address lock table.
 *   - begin(): snapshot C_start.
 *   - read(V): double-check protocol (same as WBCTL).
 *   - write(V,N): acquire lock EAGERLY on first write encounter.
 *   - commit(): increment clock, validate, write-back, unlock (atomic).
 *
 * Key difference from WBCTL: locks are acquired at write-time,
 * not deferred to commit. This provides early write-write conflict
 * detection.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Thread, Addr, MAX_VAL, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME MAX_VAL \in Nat

(* --algorithm TinySTM_WBETL

variables
    clock = 0,
    lock = [a \in Addr |-> <<0, 0, 0>>],
    mem = [a \in Addr |-> 0],
    state = [t \in Thread |-> "idle"],
    readSet = [t \in Thread |-> {}],
    writeSet = [t \in Thread |-> {}],
    writeBuf = [t \in Thread, a \in Addr |-> 0],
    readOnly = [t \in Thread |-> TRUE],
    committed = [t \in Thread |-> 0];

process ThreadProc \in Thread
begin

L_idle:
    if committed[self] < MaxCommits then
        state[self] := "active";
        readSet[self] := {};
        writeSet[self] := {};
        readOnly[self] := TRUE;
    else
        goto L_done;
    end if;

L_active:
    either \* Read (record observed version)
        with a \in Addr do
            if a \notin writeSet[self] /\ lock[a][1] = 0 then
                readSet[self] := readSet[self] \union {<<a, lock[a][3]>>};
            end if;
        end with;
        goto L_active;
    or \* New write: acquire lock and buffer
        with a \in Addr, n \in 0..MAX_VAL do
            if a \notin writeSet[self] /\ lock[a][1] = 0 then
                lock[a] := <<1, self, lock[a][3]>>;
                writeSet[self] := writeSet[self] \union {a};
                writeBuf[self, a] := n;
                readOnly[self] := FALSE;
            end if;
        end with;
        goto L_active;
    or \* Update write: buffer updated value (lock already held)
        with a \in Addr, n \in 0..MAX_VAL do
            if a \in writeSet[self] then
                writeBuf[self, a] := n;
                readOnly[self] := FALSE;
            end if;
        end with;
        goto L_active;
    or \* Write conflict abort
        if \E a \in Addr \ writeSet[self] : lock[a][1] = 1 /\ lock[a][2] # self then
            state[self] := "idle";
            readSet[self] := {};
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Commit read-only
        if readOnly[self] then
            committed[self] := committed[self] + 1;
            state[self] := "idle";
            readSet[self] := {};
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Commit (full commit: validate + write-back + unlock)
        if ~readOnly[self] /\ writeSet[self] # {}
           /\ \A a \in writeSet[self] : lock[a][2] = self
        then
            if \A <<a, v>> \in readSet[self] : lock[a][2] = self \/ lock[a][3] = v
            then
                clock := clock + 1;
                mem := [a \in Addr |->
                    IF a \in writeSet[self] THEN writeBuf[self, a] ELSE mem[a]];
                lock := [a \in Addr |->
                    IF a \in writeSet[self] THEN <<0, 0, clock>> ELSE lock[a]];
                committed[self] := committed[self] + 1;
                state[self] := "idle";
                readSet[self] := {};
                writeSet[self] := {};
                goto L_idle;
            else
                lock := [a \in Addr |->
                    IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]];
                readSet[self] := {};
                writeSet[self] := {};
                state[self] := "idle";
                goto L_idle;
            end if;
        else
            goto L_active;
        end if;
    end either;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "8b8fa9d2" /\ chksum(tla) = "93b3fd2a")
VARIABLES pc, clock, lock, mem, state, readSet, writeSet, writeBuf, readOnly, 
          committed

vars == << pc, clock, lock, mem, state, readSet, writeSet, writeBuf, readOnly, 
           committed >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ clock = 0
        /\ lock = [a \in Addr |-> <<0, 0, 0>>]
        /\ mem = [a \in Addr |-> 0]
        /\ state = [t \in Thread |-> "idle"]
        /\ readSet = [t \in Thread |-> {}]
        /\ writeSet = [t \in Thread |-> {}]
        /\ writeBuf = [t \in Thread, a \in Addr |-> 0]
        /\ readOnly = [t \in Thread |-> TRUE]
        /\ committed = [t \in Thread |-> 0]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] < MaxCommits
                      THEN /\ state' = [state EXCEPT ![self] = "active"]
                           /\ readSet' = [readSet EXCEPT ![self] = {}]
                           /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                           /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_done"]
                           /\ UNCHANGED << state, readSet, writeSet, readOnly >>
                /\ UNCHANGED << clock, lock, mem, writeBuf, committed >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \notin writeSet[self] /\ lock[a][1] = 0
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, lock[a][3]>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, mem, state, writeSet, writeBuf, readOnly, committed>>
                     \/ /\ \E a \in Addr:
                             \E n \in 0..MAX_VAL:
                               IF a \notin writeSet[self] /\ lock[a][1] = 0
                                  THEN /\ lock' = [lock EXCEPT ![a] = <<1, self, lock[a][3]>>]
                                       /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \union {a}]
                                       /\ writeBuf' = [writeBuf EXCEPT ![self, a] = n]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << lock, writeSet, 
                                                       writeBuf, readOnly >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, mem, state, readSet, committed>>
                     \/ /\ \E a \in Addr:
                             \E n \in 0..MAX_VAL:
                               IF a \in writeSet[self]
                                  THEN /\ writeBuf' = [writeBuf EXCEPT ![self, a] = n]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << writeBuf, readOnly >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, mem, state, readSet, writeSet, committed>>
                     \/ /\ IF \E a \in Addr \ writeSet[self] : lock[a][1] = 1 /\ lock[a][2] # self
                              THEN /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet >>
                        /\ UNCHANGED <<clock, lock, mem, writeSet, writeBuf, readOnly, committed>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, committed >>
                        /\ UNCHANGED <<clock, lock, mem, writeSet, writeBuf, readOnly>>
                     \/ /\ IF ~readOnly[self] /\ writeSet[self] # {}
                              /\ \A a \in writeSet[self] : lock[a][2] = self
                              THEN /\ IF \A <<a, v>> \in readSet[self] : lock[a][2] = self \/ lock[a][3] = v
                                         THEN /\ clock' = clock + 1
                                              /\ mem' =    [a \in Addr |->
                                                        IF a \in writeSet[self] THEN writeBuf[self, a] ELSE mem[a]]
                                              /\ lock' =     [a \in Addr |->
                                                         IF a \in writeSet[self] THEN <<0, 0, clock'>> ELSE lock[a]]
                                              /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                              /\ state' = [state EXCEPT ![self] = "idle"]
                                              /\ readSet' = [readSet EXCEPT ![self] = {}]
                                              /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                         ELSE /\ lock' =     [a \in Addr |->
                                                         IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]]
                                              /\ readSet' = [readSet EXCEPT ![self] = {}]
                                              /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                              /\ state' = [state EXCEPT ![self] = "idle"]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                              /\ UNCHANGED << clock, mem, 
                                                              committed >>
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << clock, lock, mem, state, 
                                                   readSet, writeSet, 
                                                   committed >>
                        /\ UNCHANGED <<writeBuf, readOnly>>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, lock, mem, state, readSet, writeSet, 
                                writeBuf, readOnly, committed >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Bounds for model checking                                          *)
(*====================================================================*)
ClockBound == clock < 4
CommBound == \A t \in Thread : committed[t] <= MaxCommits

(*====================================================================*)
(* INVARIANTS                                                         *)
(*====================================================================*)

(* No two threads hold the same lock *)
MutexLocks ==
    \A a \in Addr, t1, t2 \in Thread :
        (t1 # t2 /\ lock[a][1] = 1)
        => ~ (lock[a][2] = t1 /\ lock[a][2] = t2)

(* Lock owner is in a transaction with the address in write-set *)
LockOwnerTx ==
    \A a \in Addr :
        lock[a][1] = 1
        => \E t \in Thread :
            lock[a][2] = t /\ a \in writeSet[t]

(* No thread holds locks after commit *)
NoLocksAfterCommit ==
    \A t \in Thread : (state[t] = "idle") => \A a \in Addr : ~(lock[a][2] = t)

Inv ==
    /\ MutexLocks
    /\ LockOwnerTx
    /\ NoLocksAfterCommit

THEOREM Spec => []Inv

=======================================================================
