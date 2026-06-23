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

EXTENDS Naturals, FiniteSets, TLC

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
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0];

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
    either \* Read (record observed version + incarnation)
        with a \in Addr do
            if a \notin writeSet[self] /\ lock[a][1] = 0 then
                readSet[self] := readSet[self] \union {<<a, lock[a][3], lock[a][4]>>};
            end if;
        end with;
        goto L_active;
    or \* First write: acquire lock, write through, log old value
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
    or \* Commit: validate + release locks
        if ~readOnly[self] /\ writeSet[self] # {}
           /\ \A a \in writeSet[self] : lock[a][2] = self
        then
            if \A <<a, v, i>> \in readSet[self] :
                lock[a][2] = self \/ (lock[a][3] = v /\ lock[a][4] = i)
            then
                clock := clock + 1;
                lock := [a \in Addr |->
                    IF a \in writeSet[self] THEN <<0, 0, clock, 0>> ELSE lock[a]];
                committed[self] := committed[self] + 1;
                state[self] := "idle";
                readSet[self] := {};
                writeSet[self] := {};
                goto L_idle;
            else
                \* Validation failed: abort
                goto L_abort;
            end if;
        else
            goto L_active;
        end if;
    or \* Abort: restore from undo log, release locks
        if writeSet[self] # {} then
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

L_abort:
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
\* BEGIN TRANSLATION (chksum(pcal) = "221f6d30" /\ chksum(tla) = "b2b978b8")
VARIABLES pc, clock, lock, mem, state, readSet, writeSet, undoLog, readOnly, 
          committed, aborted

vars == << pc, clock, lock, mem, state, readSet, writeSet, undoLog, readOnly, 
           committed, aborted >>

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
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
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
                /\ UNCHANGED << clock, lock, mem, undoLog, committed, aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \notin writeSet[self] /\ lock[a][1] = 0
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, lock[a][3], lock[a][4]>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, mem, state, writeSet, undoLog, readOnly, committed, aborted>>
                     \/ /\ \E a \in Addr:
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
                        /\ UNCHANGED <<clock, state, readSet, committed, aborted>>
                     \/ /\ \E a \in Addr:
                             \E n \in 0..MAX_VAL:
                               IF a \in writeSet[self]
                                  THEN /\ undoLog' = [undoLog EXCEPT ![self, a] = mem[a]]
                                       /\ mem' = [mem EXCEPT ![a] = n]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << mem, undoLog, readOnly >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, lock, state, readSet, writeSet, committed, aborted>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, committed >>
                        /\ UNCHANGED <<clock, lock, mem, writeSet, undoLog, readOnly, aborted>>
                     \/ /\ IF ~readOnly[self] /\ writeSet[self] # {}
                              /\ \A a \in writeSet[self] : lock[a][2] = self
                              THEN /\ IF \A <<a, v, i>> \in readSet[self] :
                                          lock[a][2] = self \/ (lock[a][3] = v /\ lock[a][4] = i)
                                         THEN /\ clock' = clock + 1
                                              /\ lock' =     [a \in Addr |->
                                                         IF a \in writeSet[self] THEN <<0, 0, clock', 0>> ELSE lock[a]]
                                              /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                              /\ state' = [state EXCEPT ![self] = "idle"]
                                              /\ readSet' = [readSet EXCEPT ![self] = {}]
                                              /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                         ELSE /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                                              /\ UNCHANGED << clock, lock, 
                                                              state, readSet, 
                                                              writeSet, 
                                                              committed >>
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << clock, lock, state, readSet, 
                                                   writeSet, committed >>
                        /\ UNCHANGED <<mem, undoLog, readOnly, aborted>>
                     \/ /\ IF writeSet[self] # {}
                              THEN /\ mem' =    [a \in Addr |->
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
                                                   writeSet, aborted >>
                        /\ UNCHANGED <<clock, undoLog, readOnly, committed>>

L_abort(self) == /\ pc[self] = "L_abort"
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
                 /\ UNCHANGED << clock, undoLog, readOnly, committed >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, lock, mem, state, readSet, writeSet, 
                                undoLog, readOnly, committed, aborted >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_abort(self)
                       \/ L_done(self)

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

Inv ==
    /\ MutexLocks

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
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_begin", "L_done"})

=======================================================================
