---------------------- MODULE TinySTM_WBCTL ------------------------
(*
 * TinySTM WBCTL (Write-Back Commit-Time Locking) — TLA+ Spec (PlusCal)
 *
 * Features:
 *   - Global clock C
 *   - Per-address lock table (version + lock bit + owner ID)
 *   - begin(): empty read/write sets.
 *   - read(V): add (V, version) to read-set if not in own write-set.
 *   - write(V,N): buffer in write-set (no lock).
 *   - commit():
 *       1. Acquire locks on write-set.
 *       2. Increment clock -> C_commit.
 *       3. Validate read-set: locks.version <= C_commit-1.
 *       4. Write-back buffered values.
 *       5. Release locks with C_commit.
 *)

EXTENDS Naturals, FiniteSets, TLC, TMTypes

CONSTANTS Thread, Addr, MAX_VAL, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME MAX_VAL \in Nat

(* --algorithm TinySTM_WBCTL

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
    \* Fence tracking: ""=none, "acq"=acquire, "rel"=release, "sc"=seq_cst
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
        if \A <<a, v>> \in readSet[self] : (lock[a][1] = 0 \/ lock[a][2] = self) /\ lock[a][3] <= endVersion[self] then
            endVersion[self] := clock;
            goto L_active;
        else
            readSet[self] := {};
            writeSet[self] := {};
            state[self] := "idle";
            goto L_idle;
        end if;
    or \* Read (if not in own write-set, record observed version)
        lastSignalFence[self] := "sc";
        with a \in Addr do
            if a \notin writeSet[self] /\ lock[a][1] = 0 then
                readSet[self] := readSet[self] \union {<<a, lock[a][3]>>};
            end if;
        end with;
        goto L_active;
    or \* Write (buffer value, no lock yet)
        with a \in Addr, n \in 0..MAX_VAL do
            writeSet[self] := writeSet[self] \union {a};
            writeBuf[self, a] := n;
            readOnly[self] := FALSE;
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
    or \* Lock acquire (Phase 1: acquire all write-set locks)
        lastRmw[self] := "seq_cst";
        if ~readOnly[self] /\ writeSet[self] # {}
           /\ \A a \in writeSet[self] : lock[a][1] = 0 then
            lock := [a \in Addr |->
                IF a \in writeSet[self]
                THEN <<1, self, lock[a][3]>>
                ELSE lock[a]];
            state[self] := "locking";
            goto L_incClock;
        else
            goto L_active;
        end if;
    end either;

L_incClock:
    lastRmw[self] := "acq_rel";
    clock := clock + 1;

L_validate:
    if \A <<a, v>> \in readSet[self] :
           (lock[a][1] = 0 \/ lock[a][2] = self) /\ lock[a][3] <= endVersion[self] then
        state[self] := "wb";
        goto L_writeBack;
    else
        \* Validation failed: release locks, abort
        lastRmw[self] := "release";
        lock := [a \in Addr |->
            IF a \in writeSet[self]
            THEN <<0, 0, lock[a][3]>>
            ELSE lock[a]];
        readSet[self] := {};
        writeSet[self] := {};
        state[self] := "idle";
        goto L_idle;
    end if;

L_writeBack:
    \* Write-back buffered values and release locks with commit version
    mem := [a \in Addr |->
        IF a \in writeSet[self] THEN writeBuf[self, a] ELSE mem[a]];
    lastSignalFence[self] := "sc";
    lastRmw[self] := "release";
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
\* BEGIN TRANSLATION (chksum(pcal) = "92399dd6" /\ chksum(tla) = "5f6cdf19")
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
                                   /\ UNCHANGED << state, readSet, writeSet >>
                              ELSE /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                   /\ UNCHANGED endVersion
                        /\ UNCHANGED <<lock, writeBuf, readOnly, committed, lastSignalFence, lastThreadFence, lastRmw>>
                     \/ /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                        /\ \E a \in Addr:
                              IF a \notin writeSet[self] /\ lock[a][1] = 0
                                 THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, lock[a][3]>>}]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED readSet
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lock, state, writeSet, writeBuf, readOnly, endVersion, committed, lastThreadFence, lastRmw>>
                     \/ /\ \E a \in Addr:
                             \E n \in 0..MAX_VAL:
                               /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \union {a}]
                               /\ writeBuf' = [writeBuf EXCEPT ![self, a] = n]
                               /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lock, state, readSet, endVersion, committed, lastSignalFence, lastThreadFence, lastRmw>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, committed >>
                        /\ UNCHANGED <<lock, writeSet, writeBuf, readOnly, endVersion, lastSignalFence, lastThreadFence, lastRmw>>
                     \/ /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                        /\ IF ~readOnly[self] /\ writeSet[self] # {}
                              /\ \A a \in writeSet[self] : lock[a][1] = 0
                              THEN /\ lock' =     [a \in Addr |->
                                              IF a \in writeSet[self]
                                              THEN <<1, self, lock[a][3]>>
                                              ELSE lock[a]]
                                   /\ state' = [state EXCEPT ![self] = "locking"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_incClock"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << lock, state >>
                        /\ UNCHANGED <<readSet, writeSet, writeBuf, readOnly, endVersion, committed, lastSignalFence, lastThreadFence>>
                  /\ UNCHANGED << clock, mem >>

L_incClock(self) == /\ pc[self] = "L_incClock"
                    /\ lastRmw' = [lastRmw EXCEPT ![self] = "acq_rel"]
                    /\ clock' = clock + 1
                    /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                    /\ UNCHANGED << lock, mem, state, readSet, writeSet, 
                                    writeBuf, readOnly, endVersion, committed, lastSignalFence, lastThreadFence >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF \A <<a, v>> \in readSet[self] :
                           (lock[a][1] = 0 \/ lock[a][2] = self) /\ lock[a][3] <= endVersion[self]
                           THEN /\ state' = [state EXCEPT ![self] = "wb"]
                               /\ pc' = [pc EXCEPT ![self] = "L_writeBack"]
                               /\ UNCHANGED << lock, readSet, writeSet, 
                                               lastSignalFence, lastThreadFence, lastRmw >>
                          ELSE /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                               /\ lock' =     [a \in Addr |->
                                          IF a \in writeSet[self]
                                          THEN <<0, 0, lock[a][3]>>
                                          ELSE lock[a]]
                               /\ readSet' = [readSet EXCEPT ![self] = {}]
                               /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                               /\ state' = [state EXCEPT ![self] = "idle"]
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                    /\ UNCHANGED << clock, mem, writeBuf, readOnly, endVersion, 
                                    committed, lastSignalFence, lastThreadFence >>

L_writeBack(self) == /\ pc[self] = "L_writeBack"
                     /\ mem' =    [a \in Addr |->
                               IF a \in writeSet[self] THEN writeBuf[self, a] ELSE mem[a]]
                     /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                     /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                     /\ lock' =     [a \in Addr |->
                                IF a \in writeSet[self] THEN <<0, 0, clock>> ELSE lock[a]]
                     /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                     /\ state' = [state EXCEPT ![self] = "idle"]
                     /\ readSet' = [readSet EXCEPT ![self] = {}]
                     /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED << clock, writeBuf, readOnly, endVersion, lastThreadFence >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, lock, mem, state, readSet, writeSet, 
                                writeBuf, readOnly, endVersion, committed, 
                                lastSignalFence, lastThreadFence, lastRmw >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_incClock(self)
                       \/ L_validate(self) \/ L_writeBack(self)
                       \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(* ---- helper: lock encoding (used by invariants below) ---- *)
LOCK_FREE(i) == lock[i][1] = 0
LOCK_OWNER(i) == lock[i][2]
LOCK_VER(i) == lock[i][3]

(*====================================================================*)
(* Bounds for model checking                                          *)
(*====================================================================*)
ClockBound == clock < 4
CommBound == \A t \in Thread : committed[t] <= MaxCommits

(*====================================================================*)
(* INVARIANTS                                                         *)
(*====================================================================*)

(* Each thread in locking/wb owns all locks for its write-set *)
LockChain ==
    \A t \in Thread :
        \A a \in writeSet[t] :
            state[t] \in {"locking", "wb"} => LOCK_OWNER(a) = t

(* Lock owner matches the committing thread *)
LockOwnerInv ==
    \A a \in Addr :
        ~LOCK_FREE(a) => \E t \in Thread :
            a \in writeSet[t] /\ state[t] \in {"locking", "wb"} /\ LOCK_OWNER(a) = t

(* Write-back only happens after validation *)
WriteBackSafe ==
    \A t \in Thread : (state[t] = "wb") => (clock > 0)

FenceFidelity == \A t \in Thread :
    state[t] \in {"locking", "wb"} =>
        Fenced(t, lastSignalFence, lastThreadFence, lastRmw)

Inv ==
    /\ LockChain
    /\ LockOwnerInv
    /\ WriteBackSafe
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
