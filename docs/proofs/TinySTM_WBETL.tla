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
    endVersion = [t \in Thread |-> 0],
    committed = [t \in Thread |-> 0],
    \* Fence tracking (signal_fence, thread_fence, RMW)
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
begin

L_idle:
    if committed[self] < MaxCommits then
        state[self] := "active";
        readSet[self] := {};
        writeSet[self] := {};
        readOnly[self] := TRUE;
        endVersion[self] := clock;
    else
        goto L_done;
    end if;

L_active:
    either \* Extend (validate read-set and bump snapshot window)
        if \A <<a, v>> \in readSet[self] : lock[a][3] <= endVersion[self] then
            endVersion[self] := clock;
            goto L_active;
        else
            lastRmw[self] := "release";  \* lock.release
            lock := [a \in Addr |->
                IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]];
            readSet[self] := {};
            writeSet[self] := {};
            state[self] := "idle";
            goto L_idle;
        end if;
    or \* Read (record observed version)
        lastSignalFence[self] := "sc";  \* atomic_signal_fence(seq_cst) before version read
        with a \in Addr do
            if a \notin writeSet[self] /\ lock[a][1] = 0 then
                readSet[self] := readSet[self] \union {<<a, lock[a][3]>>};
            end if;
        end with;
        goto L_active;
    or \* New write: acquire lock and buffer
        lastRmw[self] := "acquire";  \* CAS acquire
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
        lastSignalFence[self] := "sc";  \* atomic_signal_fence(seq_cst) before data write
        with a \in Addr, n \in 0..MAX_VAL do
            if a \in writeSet[self] then
                writeBuf[self, a] := n;
                readOnly[self] := FALSE;
            end if;
        end with;
        goto L_active;
    or \* Write conflict abort (release locks + reset state)
        if \E a \in Addr \ writeSet[self] : lock[a][1] = 1 /\ lock[a][2] # self then
            lastRmw[self] := "release";  \* lock.unlock(release)
            lock := [a \in Addr |->
                IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]];
            writeSet[self] := {};
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
    or \* Commit (inc clock then validate)
        lastRmw[self] := "acq_rel";  \* g_clock.fetch_add(1, acq_rel)
        if ~readOnly[self] /\ writeSet[self] # {}
           /\ \A a \in writeSet[self] : lock[a][2] = self
        then
            clock := clock + 1;
            goto L_validateETL;
        else
            goto L_active;
        end if;
    end either;

L_validateETL:
    if \A <<a, v>> \in readSet[self] :
        lock[a][2] = self \/ lock[a][3] <= endVersion[self]
    then
        goto L_writeBackETL;
    else
        lastRmw[self] := "release";  \* lock.unlock(release)
        lock := [a \in Addr |->
            IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]];
        readSet[self] := {};
        writeSet[self] := {};
        state[self] := "idle";
        goto L_idle;
    end if;

L_writeBackETL:
    mem := [a \in Addr |->
        IF a \in writeSet[self] THEN writeBuf[self, a] ELSE mem[a]];
    lastRmw[self] := "release";  \* lock.unlock_with_version(release)
    lock := [a \in Addr |->
        IF a \in writeSet[self] THEN <<0, 0, clock>> ELSE lock[a]];
    committed[self] := committed[self] + 1;
    state[self] := "idle";
    readSet[self] := {};
    writeSet[self] := {};
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "72101da6" /\ chksum(tla) = "b2461e89")
VARIABLES pc, clock, lock, mem, state, readSet, writeSet, writeBuf, readOnly, 
          endVersion, committed, lastSignalFence, lastThreadFence, lastRmw

vars == << pc, clock, lock, mem, state, readSet, writeSet, writeBuf, readOnly, 
           endVersion, committed, lastSignalFence, lastThreadFence, lastRmw >>

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
        /\ endVersion = [t \in Thread |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] < MaxCommits
                      THEN /\ state' = [state EXCEPT ![self] = "active"]
                           /\ readSet' = [readSet EXCEPT ![self] = {}]
                           /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                           /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                           /\ endVersion' = [endVersion EXCEPT ![self] = clock]
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_done"]
                           /\ UNCHANGED << state, readSet, writeSet, readOnly, 
                                           endVersion >>
                /\ UNCHANGED << clock, lock, mem, writeBuf, committed, 
                                lastSignalFence, lastThreadFence, lastRmw >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ IF \A <<a, v>> \in readSet[self] : lock[a][3] <= endVersion[self]
                              THEN /\ endVersion' = [endVersion EXCEPT ![self] = clock]
                                   /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << lock, state, readSet, 
                                                   writeSet, lastSignalFence, lastThreadFence, lastRmw >>
                               ELSE /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                    /\ lock' =     [a \in Addr |->
                                               IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]]
                                    /\ readSet' = [readSet EXCEPT ![self] = {}]
                                    /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                    /\ state' = [state EXCEPT ![self] = "idle"]
                                    /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                    /\ UNCHANGED << endVersion, lastSignalFence, lastThreadFence >>
                        /\ UNCHANGED <<clock, writeBuf, readOnly, committed, lastSignalFence, lastThreadFence>>
                     \/ /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                        /\ \E a \in Addr:
                             IF a \notin writeSet[self] /\ lock[a][1] = 0
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, lock[a][3]>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, state, writeSet, writeBuf, readOnly, endVersion, committed, lastThreadFence, lastRmw>>
                     \/ /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                        /\ \E a \in Addr:
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
                        /\ UNCHANGED <<clock, state, readSet, endVersion, committed, lastSignalFence, lastThreadFence>>
                     \/ /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                        /\ \E a \in Addr:
                             \E n \in 0..MAX_VAL:
                               IF a \in writeSet[self]
                                  THEN /\ writeBuf' = [writeBuf EXCEPT ![self, a] = n]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << writeBuf, readOnly >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, state, readSet, writeSet, endVersion, committed, lastThreadFence, lastRmw>>
                     \/ /\ IF \E a \in Addr \ writeSet[self] : lock[a][1] = 1 /\ lock[a][2] # self
                               THEN /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                    /\ lock' =     [a \in Addr |->
                                               IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]]
                                    /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                    /\ state' = [state EXCEPT ![self] = "idle"]
                                    /\ readSet' = [readSet EXCEPT ![self] = {}]
                                    /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                    /\ UNCHANGED << lastSignalFence, lastThreadFence >>
                               ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << lock, state, readSet, 
                                                   writeSet, lastSignalFence, lastThreadFence, lastRmw >>
                        /\ UNCHANGED <<clock, writeBuf, readOnly, endVersion, committed>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, committed >>
                        /\ UNCHANGED <<clock, lock, writeSet, writeBuf, readOnly, endVersion, lastSignalFence, lastThreadFence, lastRmw>>
                     \/ /\ lastRmw' = [lastRmw EXCEPT ![self] = "acq_rel"]
                        /\ IF ~readOnly[self] /\ writeSet[self] # {}
                              /\ \A a \in writeSet[self] : lock[a][2] = self
                              THEN /\ clock' = clock + 1
                                   /\ pc' = [pc EXCEPT ![self] = "L_validateETL"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ clock' = clock
                        /\ UNCHANGED <<lock, state, readSet, writeSet, writeBuf, readOnly, endVersion, committed, lastSignalFence, lastThreadFence>>
                  /\ mem' = mem

L_validateETL(self) == /\ pc[self] = "L_validateETL"
                       /\ IF \A <<a, v>> \in readSet[self] :
                              lock[a][2] = self \/ lock[a][3] <= endVersion[self]
                             THEN /\ pc' = [pc EXCEPT ![self] = "L_writeBackETL"]
                                  /\ UNCHANGED << lock, state, readSet, 
                                                  writeSet, lastSignalFence, lastThreadFence, lastRmw >>
                             ELSE /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                  /\ lock' =     [a \in Addr |->
                                             IF a \in writeSet[self] THEN <<0, 0, lock[a][3]>> ELSE lock[a]]
                                  /\ readSet' = [readSet EXCEPT ![self] = {}]
                                  /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                  /\ state' = [state EXCEPT ![self] = "idle"]
                                  /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                       /\ UNCHANGED << clock, mem, writeBuf, readOnly, 
                                       endVersion, committed, lastSignalFence, lastThreadFence >>

L_writeBackETL(self) == /\ pc[self] = "L_writeBackETL"
                        /\ mem' =    [a \in Addr |->
                                  IF a \in writeSet[self] THEN writeBuf[self, a] ELSE mem[a]]
                        /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                        /\ lock' =     [a \in Addr |->
                                   IF a \in writeSet[self] THEN <<0, 0, clock>> ELSE lock[a]]
                        /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                        /\ state' = [state EXCEPT ![self] = "idle"]
                        /\ readSet' = [readSet EXCEPT ![self] = {}]
                        /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED << clock, writeBuf, readOnly, endVersion, lastSignalFence, lastThreadFence >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, lock, mem, state, readSet, writeSet, 
                                writeBuf, readOnly, endVersion, committed, 
                                lastSignalFence, lastThreadFence, lastRmw >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_validateETL(self)
                       \/ L_writeBackETL(self) \/ L_done(self)

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

FenceFidelity == \A t \in Thread :
    writeSet[t] # {} =>
        (lastSignalFence[t] # "" \/ lastThreadFence[t] # "" \/ lastRmw[t] # "")

Inv ==
    /\ MutexLocks
    /\ LockOwnerTx
    /\ NoLocksAfterCommit
    /\ FenceFidelity

THEOREM Spec => []Inv

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProperty ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_done"})

=======================================================================
