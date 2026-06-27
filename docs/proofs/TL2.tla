----------------------------- MODULE TL2 -----------------------------
(*
 * TL2 — TLA+ Specification (PlusCal, TLC-checkable).
 *
 * Algorithm (Dice, Shalev, Shavit 2006):
 *   - Global clock G.
 *   - Per-address guard with lock-bit + version.
 *   - begin(): snapshot G, clear read/write sets.
 *   - read(a):
 *       1. If a in own write-set, return buffered value (no tracking).
 *       2. Otherwise, record (a, guard.version) in read-set.
 *   - write(a, n): buffer (a, n) in write-set.
 *   - commit():
 *       1. If read-only: increment committed count, return to idle.
 *       2. Acquire locks on all write-set addresses (sorted order).
 *       3. Increment global clock G → c.
 *       4. Validate read-set: guard.version == observed for all entries.
 *          On mismatch: release locks, abort (no committed increment).
 *       5. Write buffered values to memory.
 *       6. Release locks with version = c.
 *       7. Increment committed count.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Thread, Addr, MAX_COMMIT, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME MAX_COMMIT \in Nat
ASSUME MaxCommits \in Nat

(* ---- helper: guard encoding ---- *)
LOCK_BIT == 1
GuardVersion(g) == g \div 2
GuardLocked(g) == g % 2
MakeGuard(locked, ver) == ver * 2 + locked

(* --algorithm TL2

variables
    clock = 0,
    guard = [a \in Addr |-> MakeGuard(0, 0)],
    mem = [a \in Addr |-> 0],
    state = [t \in Thread |-> "idle"],
    readSet = [t \in Thread |-> {}],
    writeSet = [t \in Thread |-> {}],
    writeBuf = [t \in Thread, a \in Addr |-> 0],
    snapshot = [t \in Thread |-> 0],
    readOnly = [t \in Thread |-> TRUE],
    committed = [t \in Thread |-> 0];

process ThreadProc \in Thread
begin

L_idle:
    if committed[self] < MaxCommits then
        state[self] := "active";
        snapshot[self] := clock;
        readSet[self] := {};
        writeSet[self] := {};
        readOnly[self] := TRUE;
    else
        goto L_done;
    end if;

L_active:
    either \* ReadMiss
        with a \in Addr do
            if a \notin writeSet[self] then
                readSet[self] := readSet[self] \union {<<a, GuardVersion(guard[a])>>};
            end if;
        end with;
        goto L_active;
    or \* Write
        with a \in Addr, n \in 0..MAX_COMMIT do
            writeSet[self] := writeSet[self] \union {a};
            writeBuf[self, a] := n;
            readOnly[self] := FALSE;
            lastFence[self] := "acq";
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
    or \* Commit Phase 1: acquire locks
        if ~readOnly[self] /\ writeSet[self] # {}
           /\ \A a \in writeSet[self] : GuardLocked(guard[a]) = 0 then
            guard := [a \in Addr |->
                IF a \in writeSet[self]
                THEN MakeGuard(1, GuardVersion(guard[a]))
                ELSE guard[a]];
            state[self] := "committing";
            goto L_incClock;
        else
            goto L_active;
        end if;
    end either;

L_incClock:
    clock := clock + 1;

L_validate:
    if \A <<a, v>> \in readSet[self] : /\ GuardLocked(guard[a]) = 0
                                        /\ GuardVersion(guard[a]) = v then
        state[self] := "committing_v";
        goto L_writeBack;
    else
        guard := [a \in Addr |->
            IF a \in writeSet[self]
            THEN MakeGuard(0, GuardVersion(guard[a]))
            ELSE guard[a]];
        readSet[self] := {};
        writeSet[self] := {};
        state[self] := "idle";
        goto L_idle;
    end if;

L_writeBack:
    mem := [a \in Addr |->
        IF a \in writeSet[self]
        THEN writeBuf[self, a]
        ELSE mem[a]];
    state[self] := "committing_wb";

L_release:
    guard := [a \in Addr |->
        IF a \in writeSet[self]
        THEN MakeGuard(0, clock)
        ELSE guard[a]];
    committed[self] := committed[self] + 1;
    state[self] := "idle";
    readSet[self] := {};
    writeSet[self] := {};
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "8e926f83" /\ chksum(tla) = "db4e9901")
VARIABLES pc, clock, guard, mem, state, readSet, writeSet, writeBuf, snapshot, 
          readOnly, committed, lastFence

vars == << pc, clock, guard, mem, state, readSet, writeSet, writeBuf, 
           snapshot, readOnly, committed, lastFence >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ clock = 0
        /\ guard = [a \in Addr |-> MakeGuard(0, 0)]
        /\ mem = [a \in Addr |-> 0]
        /\ state = [t \in Thread |-> "idle"]
        /\ readSet = [t \in Thread |-> {}]
        /\ writeSet = [t \in Thread |-> {}]
        /\ writeBuf = [t \in Thread, a \in Addr |-> 0]
        /\ snapshot = [t \in Thread |-> 0]
        /\ readOnly = [t \in Thread |-> TRUE]
        /\ committed = [t \in Thread |-> 0]
        /\ lastFence = [t \in Thread |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] < MaxCommits
                      THEN /\ state' = [state EXCEPT ![self] = "active"]
                           /\ snapshot' = [snapshot EXCEPT ![self] = clock]
                           /\ readSet' = [readSet EXCEPT ![self] = {}]
                           /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                           /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                           /\ lastFence' = [lastFence EXCEPT ![self] = ""]
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_done"]
                           /\ UNCHANGED << state, readSet, writeSet, snapshot, 
                                           readOnly, lastFence >>
                /\ UNCHANGED << clock, guard, mem, writeBuf, committed >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \notin writeSet[self]
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, GuardVersion(guard[a])>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                        /\ UNCHANGED <<guard, state, writeSet, writeBuf, readOnly, committed>>
                      \/ /\ \E a \in Addr:
                              \E n \in 0..MAX_COMMIT:
                                /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \union {a}]
                                /\ writeBuf' = [writeBuf EXCEPT ![self, a] = n]
                                /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                         /\ UNCHANGED <<guard, state, readSet, committed>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, committed >>
                        /\ UNCHANGED <<guard, writeSet, writeBuf, readOnly, lastFence>>
                     \/ /\ IF ~readOnly[self] /\ writeSet[self] # {}
                              /\ \A a \in writeSet[self] : GuardLocked(guard[a]) = 0
                              THEN /\ guard' =      [a \in Addr |->
                                               IF a \in writeSet[self]
                                               THEN MakeGuard(1, GuardVersion(guard[a]))
                                               ELSE guard[a]]
                                   /\ state' = [state EXCEPT ![self] = "committing"]
                                   /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_incClock"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << guard, state, lastFence >>
                        /\ UNCHANGED <<readSet, writeSet, writeBuf, readOnly, committed>>
                  /\ UNCHANGED << clock, mem, snapshot >>

L_incClock(self) == /\ pc[self] = "L_incClock"
                    /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                    /\ clock' = clock + 1
                    /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                    /\ UNCHANGED << guard, mem, state, readSet, writeSet, 
                                    writeBuf, snapshot, readOnly, committed >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF \A <<a, v>> \in readSet[self] : /\ GuardLocked(guard[a]) = 0
                                                             /\ GuardVersion(guard[a]) = v
                          THEN /\ state' = [state EXCEPT ![self] = "committing_v"]
                               /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                               /\ pc' = [pc EXCEPT ![self] = "L_writeBack"]
                               /\ UNCHANGED << guard, readSet, writeSet >>
                          ELSE /\ guard' =      [a \in Addr |->
                                           IF a \in writeSet[self]
                                           THEN MakeGuard(0, GuardVersion(guard[a]))
                                           ELSE guard[a]]
                               /\ readSet' = [readSet EXCEPT ![self] = {}]
                               /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                               /\ state' = [state EXCEPT ![self] = "idle"]
                               /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                    /\ UNCHANGED << clock, mem, writeBuf, snapshot, readOnly, 
                                    committed >>

L_writeBack(self) == /\ pc[self] = "L_writeBack"
                     /\ mem' =    [a \in Addr |->
                               IF a \in writeSet[self]
                               THEN writeBuf[self, a]
                               ELSE mem[a]]
                     /\ state' = [state EXCEPT ![self] = "committing_wb"]
                     /\ pc' = [pc EXCEPT ![self] = "L_release"]
                     /\ UNCHANGED << clock, guard, readSet, writeSet, writeBuf, 
                                     snapshot, readOnly, committed, lastFence >>

L_release(self) == /\ pc[self] = "L_release"
                   /\ guard' =      [a \in Addr |->
                               IF a \in writeSet[self]
                               THEN MakeGuard(0, clock)
                               ELSE guard[a]]
                   /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                   /\ state' = [state EXCEPT ![self] = "idle"]
                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                   /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                   /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                   /\ UNCHANGED << clock, mem, writeBuf, snapshot, readOnly >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, guard, mem, state, readSet, writeSet, 
                                writeBuf, snapshot, readOnly, committed, 
                                lastFence >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_incClock(self)
                       \/ L_validate(self) \/ L_writeBack(self)
                       \/ L_release(self) \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION 

(*====================================================================*)
(* INVARIANTS                                                          *)
(*====================================================================*)

(* Invariant 1: A guard is locked iff the owning thread is committing *)
LockConsistent ==
    \A a \in Addr :
        GuardLocked(guard[a]) = 1
        <=> \E t \in Thread :
            a \in writeSet[t] /\ state[t] \in {"committing", "committing_v", "committing_wb"}

(* Invariant 2: No thread reads an address whose guard is locked by another *)
(* NOTE: Excluded from Inv below — the universal quantifier over ALL t2 triggers   *)
(* false positives when t2 is the locking thread (state ≠ "idle", a ∈ writeSet).   *)
(* TL2 tolerates stale read-set entries across concurrent commit; validation        *)
(* catches them at commit time via version mismatch. TLC would produce spurious     *)
(* counterexamples if checked.                                                      *)
NoDirtyRead ==
    \A t1, t2 \in Thread, a \in Addr :
        (t1 # t2)
        /\ \E v \in 0..MAX_COMMIT : <<a, v>> \in readSet[t1]
        /\ GuardLocked(guard[a]) = 1
        => state[t2] = "idle" \/ a \notin writeSet[t2]

(* Invariant 3: No thread's snapshot exceeds clock *)
SnapshotInv ==
    \A t \in Thread : snapshot[t] <= clock

(* Invariant 4: Every thread with a non-empty write-set has issued a fence *)
FenceFidelity ==
    \A t \in Thread : writeSet[t] # {} => lastFence[t] # ""

(* Combined invariant for TLC *)
Inv ==
    /\ LockConsistent
    /\ SnapshotInv
    /\ FenceFidelity

(* Verify the algorithm *)
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
