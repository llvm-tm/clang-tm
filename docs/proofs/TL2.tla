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
 *       2. Acquire locks on write-set addresses ONE AT A TIME (sorted
 *          order in C++; non-deterministic order here).  If any address
 *          is already locked, release all acquired so far and abort.
 *       3. Increment global clock G -> c.
 *       4. Validate read-set: for entries NOT in our write-set, check
 *          guard.version == observed.  Entries we wrote are excluded
 *          (we already hold the lock).
 *       5. Write buffered values to memory.
 *       6. Release locks with version = c.
 *       7. Increment committed count.
 *
 * Fidelity notes:
 *   - Lock acquisition is incremental (per-address CAS), not atomic bulk.
 *     This matches C++ tl2.hpp which iterates write_set entries one at a
 *     time via try_acquire_guard().
 *   - lock_addrs[t] tracks addresses whose guard has been acquired in the
 *     current commit.  On any acquisition failure, all acquired guards
 *     are released and the transaction aborts.
 *)

EXTENDS Naturals, FiniteSets, TLC, TMTypes

CONSTANTS Thread, Addr, MAX_COMMIT, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME MAX_COMMIT \in Nat
ASSUME MaxCommits \in Nat

(* --algorithm TL2

variables
    clock = 0,
    guard = [a \in Addr |-> MakeEntry(0)],
    mem = [a \in Addr |-> 0],
    state = [t \in Thread |-> "idle"],
    readSet = [t \in Thread |-> {}],
    writeSet = [t \in Thread |-> {}],
    writeBuf = [t \in Thread, a \in Addr |-> 0],
    snapshot = [t \in Thread |-> 0],
    readOnly = [t \in Thread |-> TRUE],
    committed = [t \in Thread |-> 0],
    \* Tracks addresses locked so far in this commit (incremental acquisition)
    locked_addrs = [t \in Thread |-> {}],
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
begin

L_idle:
    if committed[self] < MaxCommits then
        state[self] := "active";
        snapshot[self] := clock;
        readSet[self] := {};
        writeSet[self] := {};
        locked_addrs[self] := {};
        readOnly[self] := TRUE;
        lastSignalFence[self] := "";
        lastThreadFence[self] := "";
        lastRmw[self] := "";
    else
        goto L_done;
    end if;

L_active:
    either \* ReadMiss
        with a \in Addr do
            if a \notin writeSet[self] then
                readSet[self] := readSet[self] \union {<<a, VersionOf(guard[a])>>};
            end if;
        end with;
        lastRmw[self] := "seq_cst";
        goto L_active;
    or \* Write
        with a \in Addr, n \in 0..MAX_COMMIT do
            writeSet[self] := writeSet[self] \union {a};
            writeBuf[self, a] := n;
            readOnly[self] := FALSE;
            lastRmw[self] := "acquire";
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
    or \* Commit Phase 1: start lock acquisition
        if ~readOnly[self] /\ writeSet[self] # {} then
            goto L_locking;
        else
            goto L_active;
        end if;
    end either;

L_locking:
    if locked_addrs[self] = writeSet[self] then
        \* All write-set addresses locked; proceed.
        state[self] := "committing";
        goto L_incClock;
    else
        with a \in writeSet[self] \ locked_addrs[self] do
            if LockBit(guard[a]) = 0 then
                guard[a] := MakeEntry(VersionOf(guard[a])) + 1;
                locked_addrs[self] := locked_addrs[self] \union {a};
                lastRmw[self] := "acquire";
                goto L_locking;
            else
                \* Address already locked by another thread: release all, abort
                guard := [x \in Addr |->
                    IF x \in locked_addrs[self]
                    THEN MakeEntry(VersionOf(guard[x]))
                    ELSE guard[x]];
                readSet[self] := {};
                writeSet[self] := {};
                locked_addrs[self] := {};
                state[self] := "idle";
                goto L_idle;
            end if;
        end with;
    end if;

L_incClock:
    lastRmw[self] := "release";
    clock := clock + 1;

L_validate:
    \* Check read-set entries not in our write-set (we own those guards).
    \* Our own locked guards have LockBit=1 but are safe — we control them.
    if \A <<a, v>> \in readSet[self] :
        (LockBit(guard[a]) = 0 \/ a \in locked_addrs[self]) /\
        VersionOf(guard[a]) = v
    then
        lastSignalFence[self] := "sc";
        state[self] := "committing_v";
        goto L_writeBack;
    else
        \* Release our guards, abort
        guard := [x \in Addr |->
            IF x \in locked_addrs[self]
            THEN MakeEntry(VersionOf(guard[x]))
            ELSE guard[x]];
        readSet[self] := {};
        writeSet[self] := {};
        locked_addrs[self] := {};
        lastRmw[self] := "release";
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
    lastRmw[self] := "release";
    guard := [a \in Addr |->
        IF a \in locked_addrs[self]
        THEN MakeEntry(clock)
        ELSE guard[a]];
    committed[self] := committed[self] + 1;
    state[self] := "idle";
    readSet[self] := {};
    writeSet[self] := {};
    locked_addrs[self] := {};
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "8e926f83" /\ chksum(tla) = "db4e9901")
VARIABLES pc, clock, guard, mem, state, readSet, writeSet, writeBuf, snapshot, 
          readOnly, committed, locked_addrs, lastSignalFence, lastThreadFence, lastRmw

vars == << pc, clock, guard, mem, state, readSet, writeSet, writeBuf, 
           snapshot, readOnly, committed, locked_addrs,
           lastSignalFence, lastThreadFence, lastRmw >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ clock = 0
        /\ guard = [a \in Addr |-> MakeEntry(0)]
        /\ mem = [a \in Addr |-> 0]
        /\ state = [t \in Thread |-> "idle"]
        /\ readSet = [t \in Thread |-> {}]
        /\ writeSet = [t \in Thread |-> {}]
        /\ writeBuf = [t \in Thread, a \in Addr |-> 0]
        /\ snapshot = [t \in Thread |-> 0]
        /\ readOnly = [t \in Thread |-> TRUE]
        /\ committed = [t \in Thread |-> 0]
        /\ locked_addrs = [t \in Thread |-> {}]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] < MaxCommits
                      THEN /\ state' = [state EXCEPT ![self] = "active"]
                           /\ snapshot' = [snapshot EXCEPT ![self] = clock]
                           /\ readSet' = [readSet EXCEPT ![self] = {}]
                           /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                           /\ locked_addrs' = [locked_addrs EXCEPT ![self] = {}]
                           /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                           /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                           /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                           /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_done"]
                            /\ UNCHANGED << state, readSet, writeSet, locked_addrs,
                                            snapshot, readOnly, lastSignalFence, 
                                            lastThreadFence, lastRmw >>
                /\ UNCHANGED << clock, guard, mem, writeBuf, committed >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \notin writeSet[self]
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, VersionOf(guard[a])>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                        /\ UNCHANGED <<guard, state, writeSet, writeBuf, readOnly,
                                        locked_addrs, committed>>
                      \/ /\ \E a \in Addr:
                              \E n \in 0..MAX_COMMIT:
                                /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \union {a}]
                                /\ writeBuf' = [writeBuf EXCEPT ![self, a] = n]
                                /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                         /\ UNCHANGED <<guard, state, readSet, locked_addrs, committed>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ state' = [state EXCEPT ![self] = "idle"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << state, readSet, committed >>
                        /\ UNCHANGED <<guard, writeSet, writeBuf, readOnly, locked_addrs,
                                         lastSignalFence, lastThreadFence, lastRmw>>
                     \/ /\ IF ~readOnly[self] /\ writeSet[self] # {}
                              THEN /\ pc' = [pc EXCEPT ![self] = "L_locking"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<guard, state, readSet, writeSet, writeBuf,
                                        readOnly, committed, lastSignalFence,
                                        lastThreadFence, lastRmw, locked_addrs>>
                  /\ UNCHANGED << clock, mem, snapshot >>

L_locking(self) == /\ pc[self] = "L_locking"
                   /\ IF locked_addrs[self] = writeSet[self]
                         THEN /\ state' = [state EXCEPT ![self] = "committing"]
                              /\ pc' = [pc EXCEPT ![self] = "L_incClock"]
                              /\ UNCHANGED << guard, readSet, writeSet, locked_addrs >>
                         ELSE /\ \E a \in writeSet[self] \ locked_addrs[self]:
                                   IF LockBit(guard[a]) = 0
                                      THEN /\ guard' = [guard EXCEPT ![a] = MakeEntry(VersionOf(guard[a])) + 1]
                                           /\ locked_addrs' = [locked_addrs EXCEPT ![self] = locked_addrs[self] \union {a}]
                                           /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                                           /\ pc' = [pc EXCEPT ![self] = "L_locking"]
                                           /\ UNCHANGED << state, readSet, writeSet >>
                                      ELSE /\ guard' = [x \in Addr |->
                                                   IF x \in locked_addrs[self]
                                                   THEN MakeEntry(VersionOf(guard[x]))
                                                   ELSE guard[x]]
                                           /\ readSet' = [readSet EXCEPT ![self] = {}]
                                           /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                           /\ locked_addrs' = [locked_addrs EXCEPT ![self] = {}]
                                           /\ state' = [state EXCEPT ![self] = "idle"]
                                           /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                           /\ UNCHANGED lastRmw
                   /\ UNCHANGED << clock, mem, writeBuf, snapshot, readOnly, 
                                   committed, lastSignalFence, lastThreadFence >>

L_incClock(self) == /\ pc[self] = "L_incClock"
                    /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                    /\ clock' = clock + 1
                    /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                    /\ UNCHANGED << guard, mem, state, readSet, writeSet, locked_addrs,
                                    writeBuf, snapshot, readOnly, committed, 
                                    lastSignalFence, lastThreadFence >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF \A <<a, v>> \in readSet[self] :
                           (LockBit(guard[a]) = 0 \/ a \in locked_addrs[self]) /\
                           VersionOf(guard[a]) = v
                          THEN /\ state' = [state EXCEPT ![self] = "committing_v"]
                               /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                               /\ pc' = [pc EXCEPT ![self] = "L_writeBack"]
                               /\ UNCHANGED << guard, readSet, writeSet, locked_addrs >>
                          ELSE /\ guard' = [x \in Addr |->
                                       IF x \in locked_addrs[self]
                                       THEN MakeEntry(VersionOf(guard[x]))
                                       ELSE guard[x]]
                               /\ readSet' = [readSet EXCEPT ![self] = {}]
                               /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                               /\ locked_addrs' = [locked_addrs EXCEPT ![self] = {}]
                               /\ state' = [state EXCEPT ![self] = "idle"]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                    /\ UNCHANGED << clock, mem, writeBuf, snapshot, readOnly, 
                                    committed, lastThreadFence >>

L_writeBack(self) == /\ pc[self] = "L_writeBack"
                     /\ mem' =    [a \in Addr |->
                               IF a \in writeSet[self]
                               THEN writeBuf[self, a]
                               ELSE mem[a]]
                     /\ state' = [state EXCEPT ![self] = "committing_wb"]
                     /\ pc' = [pc EXCEPT ![self] = "L_release"]
                     /\ UNCHANGED << clock, guard, readSet, writeSet, locked_addrs,
                                     writeBuf, snapshot, readOnly, committed,
                                     lastSignalFence, lastThreadFence, lastRmw >>

L_release(self) == /\ pc[self] = "L_release"
                   /\ guard' = [a \in Addr |->
                               IF a \in locked_addrs[self]
                               THEN MakeEntry(clock)
                               ELSE guard[a]]
                   /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                   /\ state' = [state EXCEPT ![self] = "idle"]
                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                   /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                   /\ locked_addrs' = [locked_addrs EXCEPT ![self] = {}]
                   /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                   /\ UNCHANGED << clock, mem, writeBuf, snapshot, readOnly >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, guard, mem, state, readSet, writeSet, 
                                writeBuf, snapshot, readOnly, committed, locked_addrs,
                                lastSignalFence, lastThreadFence, lastRmw >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_locking(self)
                       \/ L_incClock(self) \/ L_validate(self)
                       \/ L_writeBack(self) \/ L_release(self) \/ L_done(self)

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

(* Invariant 1: A guard is locked iff a committing thread has it in locked_addrs *)
LockConsistent ==
    \A a \in Addr :
        LockBit(guard[a]) = 1
        <=> \E t \in Thread :
            a \in locked_addrs[t] /\ state[t] \in {"committing", "committing_v", "committing_wb"}

(* Invariant 2: No thread's snapshot exceeds clock *)
SnapshotInv ==
    \A t \in Thread : snapshot[t] <= clock

(* Invariant 3: Every thread with a non-empty write-set has issued a fence *)
FenceFidelity == TMTypes!FenceFidelity(Thread, writeSet, lastSignalFence, lastThreadFence, lastRmw)

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
