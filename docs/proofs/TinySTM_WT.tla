----------------------- MODULE TinySTM_WT ------------------------
(*
 * TinySTM WT (Write-Through) — TLA+ Specification (PlusCal)
 *
 * Features:
 *   - Global clock C.
 *   - Per-address lock with version + incarnation bits.
 *   - Write-through: writes go directly to memory (with undo log).
 *   - Encounter-time locking (lock acquired at first write).
 *   - Undo log on abort: restore old values, bump incarnation.
 *   - begin(): empty read/write sets.
 *   - read(V): double-check, add to read-set.
 *   - write(V,N): lock(V), *V = N, log old value.
 *   - commit(): increment C, validate read-set, unlock(V, C).
 *   - abort(): restore old values, bump incarnation, unlock.
 *)

EXTENDS Naturals, FiniteSets, TLC, TMTypes

CONSTANTS Thread, Addr, MAX_VAL, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME MAX_VAL \in Nat

(* --algorithm TinySTM_WT

variables
    clock = 0,
    lock = [a \in Addr |-> <<0, 0, 0, 0>>],
    mem = [a \in Addr |-> 0],
    state = [t \in Thread |-> "idle"],
    readSet = [t \in Thread |-> {}],
    writeSet = [t \in Thread |-> {}],
    undoLog = [t \in Thread, a \in Addr |-> 0],
    readOnly = [t \in Thread |-> TRUE],
    endVersion = [t \in Thread |-> 0],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
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
        if \A <<a, v, i>> \in readSet[self] :
            lock[a][2] = self \/ (lock[a][1] = 0 /\ lock[a][3] = v /\ lock[a][4] = i)
        then
            endVersion[self] := clock;
            goto L_active;
        else
            lastRmw[self] := "release";  \* lock.release
            readSet[self] := {};
            writeSet[self] := {};
            state[self] := "idle";
            goto L_idle;
        end if;
    or \* Read (record observed version + incarnation)
        lastSignalFence[self] := "sc";  \* atomic_signal_fence(seq_cst) before version read
        with a \in Addr do
            if a \notin writeSet[self] /\ lock[a][1] = 0 then
                readSet[self] := readSet[self] \union {<<a, lock[a][3], lock[a][4]>>};
            end if;
        end with;
        goto L_active;
    or \* First write: acquire lock, write through, log old value
        lastRmw[self] := "seq_cst";  \* CAS (default seq_cst)
        with a \in Addr, n \in 0..MAX_VAL do
            if a \notin writeSet[self] /\ lock[a][1] = 0 then
                lock[a] := <<1, self, lock[a][3], lock[a][4]>>;
                undoLog[self, a] := mem[a];
                mem[a] := n;
                writeSet[self] := writeSet[self] \union {a};
                readOnly[self] := FALSE;
            end if;
        end with;
        goto L_active;
    or \* Update write: write through, log old value
        lastSignalFence[self] := "sc";  \* atomic_signal_fence(seq_cst) before in-place write
        with a \in Addr, n \in 0..MAX_VAL do
            if a \in writeSet[self] then
                undoLog[self, a] := mem[a];
                mem[a] := n;
                readOnly[self] := FALSE;
            end if;
        end with;
        goto L_active;
    or \* Commit read-only
        if readOnly[self] then
            committed[self] := committed[self] + 1;
            state[self] := "idle";
            readSet[self] := {};
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Commit (inc clock)
        lastRmw[self] := "acq_rel";  \* g_clock.fetch_add(1, acq_rel)
        if ~readOnly[self] /\ writeSet[self] # {}
           /\ \A a \in writeSet[self] : lock[a][2] = self
        then
            clock := clock + 1;
            goto L_validateWT;
        else
            goto L_active;
        end if;
    or \* Abort: restore from undo log, release locks
        if writeSet[self] # {} then
            lastRmw[self] := "release";  \* lock.unlock(release)
            mem := [a \in Addr |->
                IF a \in writeSet[self] THEN undoLog[self, a] ELSE mem[a]];
            lock := [a \in Addr |->
                IF a \in writeSet[self]
                THEN <<0, 0, lock[a][3], (lock[a][4] + 1) % 8>>
                ELSE lock[a]];
            aborted[self] := aborted[self] + 1;
            readSet[self] := {};
            writeSet[self] := {};
            state[self] := "idle";
            goto L_idle;
        else
            goto L_active;
        end if;
    end either;

L_validateWT:
    lastSignalFence[self] := "sc";  \* atomic_signal_fence(seq_cst) before validation
    if \A <<a, v, i>> \in readSet[self] :
        lock[a][2] = self \/ (lock[a][1] = 0 /\ lock[a][3] = v /\ lock[a][4] = i)
    then
        goto L_unlock;
    else
        goto L_abort;
    end if;

L_unlock:
    lastRmw[self] := "release";  \* store(release) unlock
    lock := [a \in Addr |->
        IF a \in writeSet[self] THEN <<0, 0, clock, 0>> ELSE lock[a]];
    committed[self] := committed[self] + 1;
    state[self] := "idle";
    readSet[self] := {};
    writeSet[self] := {};
    goto L_idle;

L_abort:
    lastRmw[self] := "release";  \* store(release) unlock
    mem := [a \in Addr |->
        IF a \in writeSet[self] THEN undoLog[self, a] ELSE mem[a]];
    lock := [a \in Addr |->
        IF a \in writeSet[self]
        THEN <<0, 0, lock[a][3], (lock[a][4] + 1) % 8>>
        ELSE lock[a]];
    aborted[self] := aborted[self] + 1;
    readSet[self] := {};
    writeSet[self] := {};
    state[self] := "idle";
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "80e933cf" /\ chksum(tla) = "38de78dc")
VARIABLES clock, lock, mem, state, readSet, writeSet, undoLog, readOnly, 
          endVersion, committed, aborted, lastSignalFence, lastThreadFence, 
          lastRmw, pc

vars == << clock, lock, mem, state, readSet, writeSet, undoLog, readOnly, 
           endVersion, committed, aborted, lastSignalFence, lastThreadFence, 
           lastRmw, pc >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ clock = 0
        /\ lock = [a \in Addr |-> <<0, 0, 0, 0>>]
        /\ mem = [a \in Addr |-> 0]
        /\ state = [t \in Thread |-> "idle"]
        /\ readSet = [t \in Thread |-> {}]
        /\ writeSet = [t \in Thread |-> {}]
        /\ undoLog = [t \in Thread, a \in Addr |-> 0]
        /\ readOnly = [t \in Thread |-> TRUE]
        /\ endVersion = [t \in Thread |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
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
                /\ UNCHANGED << clock, lock, mem, undoLog, committed, aborted, 
                                lastSignalFence, lastThreadFence, lastRmw >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ IF \A <<a, v, i>> \in readSet[self] :
                               lock[a][2] = self \/ (lock[a][1] = 0 /\ lock[a][3] = v /\ lock[a][4] = i)
                              THEN /\ endVersion' = [endVersion EXCEPT ![self] = clock]
                                   /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, writeSet, 
                                                   lastRmw >>
                              ELSE /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                   /\ UNCHANGED endVersion
                        /\ UNCHANGED <<clock, lock, mem, undoLog, readOnly, committed, aborted, lastSignalFence>>
                     \/ /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                        /\ \E a \in Addr:
                             IF a \notin writeSet[self] /\ lock[a][1] = 0
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, lock[a][3], lock[a][4]>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, mem, state, writeSet, undoLog, readOnly, endVersion, committed, aborted, lastRmw>>
                     \/ /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                        /\ \E a \in Addr:
                             \E n \in 0..MAX_VAL:
                               IF a \notin writeSet[self] /\ lock[a][1] = 0
                                  THEN /\ lock' = [lock EXCEPT ![a] = <<1, self, lock[a][3], lock[a][4]>>]
                                       /\ undoLog' = [undoLog EXCEPT ![self, a] = mem[a]]
                                       /\ mem' = [mem EXCEPT ![a] = n]
                                       /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \union {a}]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << lock, mem, writeSet, 
                                                       undoLog, readOnly >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, state, readSet, endVersion, committed, aborted, lastSignalFence>>
                     \/ /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                        /\ \E a \in Addr:
                             \E n \in 0..MAX_VAL:
                               IF a \in writeSet[self]
                                  THEN /\ undoLog' = [undoLog EXCEPT ![self, a] = mem[a]]
                                       /\ mem' = [mem EXCEPT ![a] = n]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << mem, undoLog, readOnly >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, state, readSet, writeSet, endVersion, committed, aborted, lastRmw>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, committed >>
                        /\ UNCHANGED <<clock, lock, mem, writeSet, undoLog, readOnly, endVersion, aborted, lastSignalFence, lastRmw>>
                     \/ /\ lastRmw' = [lastRmw EXCEPT ![self] = "acq_rel"]
                        /\ IF ~readOnly[self] /\ writeSet[self] # {}
                              /\ \A a \in writeSet[self] : lock[a][2] = self
                              THEN /\ clock' = clock + 1
                                   /\ pc' = [pc EXCEPT ![self] = "L_validateWT"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ clock' = clock
                        /\ UNCHANGED <<lock, mem, state, readSet, writeSet, undoLog, readOnly, endVersion, committed, aborted, lastSignalFence>>
                     \/ /\ IF writeSet[self] # {}
                              THEN /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                   /\ mem' =    [a \in Addr |->
                                             IF a \in writeSet[self] THEN undoLog[self, a] ELSE mem[a]]
                                   /\ lock' =     [a \in Addr |->
                                              IF a \in writeSet[self]
                                              THEN <<0, 0, lock[a][3], (lock[a][4] + 1) % 8>>
                                              ELSE lock[a]]
                                   /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << lock, mem, state, readSet, 
                                                   writeSet, aborted, lastRmw >>
                        /\ UNCHANGED <<clock, undoLog, readOnly, endVersion, committed, lastSignalFence>>
                  /\ UNCHANGED lastThreadFence

L_validateWT(self) == /\ pc[self] = "L_validateWT"
                      /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                      /\ IF \A <<a, v, i>> \in readSet[self] :
                             lock[a][2] = self \/ (lock[a][1] = 0 /\ lock[a][3] = v /\ lock[a][4] = i)
                            THEN /\ pc' = [pc EXCEPT ![self] = "L_unlock"]
                            ELSE /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                      /\ UNCHANGED << clock, lock, mem, state, readSet, 
                                      writeSet, undoLog, readOnly, endVersion, 
                                      committed, aborted, lastThreadFence, 
                                      lastRmw >>

L_unlock(self) == /\ pc[self] = "L_unlock"
                  /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                  /\ lock' =     [a \in Addr |->
                             IF a \in writeSet[self] THEN <<0, 0, clock, 0>> ELSE lock[a]]
                  /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                  /\ state' = [state EXCEPT ![self] = "idle"]
                  /\ readSet' = [readSet EXCEPT ![self] = {}]
                  /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                  /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                  /\ UNCHANGED << clock, mem, undoLog, readOnly, endVersion, 
                                  aborted, lastSignalFence, lastThreadFence >>

L_abort(self) == /\ pc[self] = "L_abort"
                 /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                 /\ mem' =    [a \in Addr |->
                           IF a \in writeSet[self] THEN undoLog[self, a] ELSE mem[a]]
                 /\ lock' =     [a \in Addr |->
                            IF a \in writeSet[self]
                            THEN <<0, 0, lock[a][3], (lock[a][4] + 1) % 8>>
                            ELSE lock[a]]
                 /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                 /\ readSet' = [readSet EXCEPT ![self] = {}]
                 /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                 /\ state' = [state EXCEPT ![self] = "idle"]
                 /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                 /\ UNCHANGED << clock, undoLog, readOnly, endVersion, 
                                 committed, lastSignalFence, lastThreadFence >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, lock, mem, state, readSet, writeSet, 
                                undoLog, readOnly, endVersion, committed, 
                                aborted, lastSignalFence, lastThreadFence, 
                                lastRmw >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_validateWT(self)
                       \/ L_unlock(self) \/ L_abort(self) \/ L_done(self)

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
ModelBound ==
    /\ clock < 4
    /\ \A t \in Thread : committed[t] <= MaxCommits
    /\ \A t \in Thread : aborted[t] < 4

(*====================================================================*)
(* INVARIANTS                                                         *)
(*====================================================================*)

(* Mutual exclusion on locks *)
MutexLocks ==
    \A a \in Addr : lock[a][1] = 0
        \/ \E t \in Thread : lock[a][2] = t

FenceFidelityInst == FenceFidelity(Thread, writeSet, lastSignalFence, lastThreadFence, lastRmw)

Inv ==
    /\ MutexLocks
    /\ FenceFidelityInst

THEOREM Spec => []Inv

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProp ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_done"})

=======================================================================
