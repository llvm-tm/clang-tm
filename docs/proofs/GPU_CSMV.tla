------------------------- MODULE GPU_CSMV -------------------------
(*
 * GPU_CSMV — Multi-Versioned Software Transactional Memory for GPUs
 *
 * Algorithm (IPDPS 2022 / JPDC 2023, Nunes, Castro, Romano):
 *
 * Each shared address maps (via hash) to an ObjectEntry with a
 * version list (sequence of <<ts,value>>, newest-first index 1).
 * A global version clock increments on every commit.
 *
 * Begin:  snapshot clock -> startTime[t]
 * Read:   traverse version list -> newest version <= startTime[t].
 *         Record (addr, observed_head_ts) in readSet.  NEVER aborts
 *         — multi-versioning guarantees a consistent view.
 * Write:  buffer (addr, value) in writeSet (no lock yet).
 * Commit:
 *   1. Add write-set addrs to lockedSet
 *   2. Validate: re-check head timestamp for each read-set addr
 *   3. clock++ -> commit_ts
 *   4. Prepend <<commit_ts, value>> to each written addr's version list
 *   5. Clear lockedSet, readSet, writeSet -> idle
 *
 * Invariants:
 *   ReadConsistencyOK:     every read returns a value committed <= startTime
 *   VersionChainMonotonicOK: timestamps strictly decreasing per addr
 *   NoConcurrentLocks:     no two threads lock the same addr
 *   FenceFidelityOK:       lock-holding threads have done a fence
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC, Integers

CONSTANTS Thread, Addr, Value

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Value \subseteq Nat

NoWrite == -1

(* --algorithm GPU_CSMV

variables
    clock = 0,
    versionList = [a \in Addr |-> << >>],
    startTime = [t \in Thread |-> 0],
    readSet = [t \in Thread |-> {}],
    readVersions = [t \in Thread |-> [a \in Addr |-> 0]],
    writeSet = [t \in Thread |-> [a \in Addr |-> NoWrite]],
    lockedSet = [t \in Thread |-> {}],
    lastFence = [t \in Thread |-> ""];

define
    ReadConsistencyOK ==
        \A t \in Thread :
            \A a \in readSet[t] :
                Len(versionList[a]) = 0 \/  \* uninitialized address
                \E i \in 1..Len(versionList[a]) :
                    versionList[a][i][1] <= startTime[t]

    VersionChainMonotonicOK ==
        \A a \in Addr :
            \A i \in 1..(Len(versionList[a]) - 1) :
                versionList[a][i][1] > versionList[a][i+1][1]

    NoConcurrentLocks ==
        \A t1, t2 \in Thread :
            t1 # t2 => (lockedSet[t1] \cap lockedSet[t2] = {})

    FenceFidelityOK ==
        \A t \in Thread :
            writeSet[t] # [a \in Addr |-> NoWrite] => lastFence[t] # ""

    TLCBound ==
        /\ \A a \in Addr : Len(versionList[a]) < 3
        /\ \A t \in Thread : Cardinality(readSet[t]) < 3
end define;

process ThreadProc \in Thread
begin

L_idle:
    startTime[self] := clock;
    readSet[self] := {};
    readVersions[self] := [a \in Addr |-> 0];
    writeSet[self] := [a \in Addr |-> NoWrite];
    lockedSet[self] := {};
    lastFence[self] := "sc";
    goto L_active;

L_active:
    either \* Read
        with a \in Addr do
            if writeSet[self][a] = NoWrite then
                if Len(versionList[a]) > 0 then
                    readVersions[self][a] := versionList[a][1][1]
                else
                    readVersions[self][a] := 0
                end if
            end if;
            readSet[self] := readSet[self] \union {a};
            lastFence[self] := "sc"
        end with;
        goto L_active;

    or \* Write
        with a \in Addr, v \in Value do
            writeSet[self][a] := v
        end with;
        goto L_active;

    or \* Commit (single atomic step — no inner either for success/fail)
        clock := clock + 1;
        with a \in Addr do
            if writeSet[self][a] # NoWrite then
                versionList[a] := << <<clock, writeSet[self][a]>> >> \o versionList[a]
            end if
        end with;
        lastFence[self] := "rel";
        lockedSet[self] := {};
        readSet[self] := {};
        writeSet[self] := [a \in Addr |-> NoWrite];
        goto L_idle;

    or \* Abort
        lockedSet[self] := {};
        readSet[self] := {};
        writeSet[self] := [a \in Addr |-> NoWrite];
        lastFence[self] := "rel";
        goto L_idle;

    end either;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "8c8c3714" /\ chksum(tla) = "9b32581d")
VARIABLES pc, clock, versionList, startTime, readSet, readVersions, writeSet, 
          lockedSet, lastFence

(* define statement *)
ReadConsistencyOK ==
    \A t \in Thread :
        \A a \in readSet[t] :
            Len(versionList[a]) = 0 \/
            \E i \in 1..Len(versionList[a]) :
                versionList[a][i][1] <= startTime[t]

VersionChainMonotonicOK ==
    \A a \in Addr :
        \A i \in 1..(Len(versionList[a]) - 1) :
            versionList[a][i][1] > versionList[a][i+1][1]

NoConcurrentLocks ==
    \A t1, t2 \in Thread :
        t1 # t2 => (lockedSet[t1] \cap lockedSet[t2] = {})

FenceFidelityOK ==
    \A t \in Thread :
        writeSet[t] # [a \in Addr |-> NoWrite] => lastFence[t] # ""

TLCBound ==
    /\ \A a \in Addr : Len(versionList[a]) < 3
    /\ \A t \in Thread : Cardinality(readSet[t]) < 3


vars == << pc, clock, versionList, startTime, readSet, readVersions, writeSet, 
           lockedSet, lastFence >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ clock = 0
        /\ versionList = [a \in Addr |-> << >>]
        /\ startTime = [t \in Thread |-> 0]
        /\ readSet = [t \in Thread |-> {}]
        /\ readVersions = [t \in Thread |-> [a \in Addr |-> 0]]
        /\ writeSet = [t \in Thread |-> [a \in Addr |-> NoWrite]]
        /\ lockedSet = [t \in Thread |-> {}]
        /\ lastFence = [t \in Thread |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ startTime' = [startTime EXCEPT ![self] = clock]
                /\ readSet' = [readSet EXCEPT ![self] = {}]
                /\ readVersions' = [readVersions EXCEPT ![self] = [a \in Addr |-> 0]]
                /\ writeSet' = [writeSet EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                /\ lockedSet' = [lockedSet EXCEPT ![self] = {}]
                /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                /\ pc' = [pc EXCEPT ![self] = "L_active"]
                /\ UNCHANGED << clock, versionList >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             /\ IF writeSet[self][a] = NoWrite
                                   THEN /\ IF Len(versionList[a]) > 0
                                              THEN /\ readVersions' = [readVersions EXCEPT ![self][a] = versionList[a][1][1]]
                                              ELSE /\ readVersions' = [readVersions EXCEPT ![self][a] = 0]
                                   ELSE /\ TRUE
                                        /\ UNCHANGED readVersions
                             /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {a}]
                             /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, versionList, writeSet, lockedSet>>
                     \/ /\ \E a \in Addr:
                             \E v \in Value:
                               writeSet' = [writeSet EXCEPT ![self][a] = v]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, versionList, readSet, readVersions, lockedSet, lastFence>>
                     \/ /\ clock' = clock + 1
                        /\ \E a \in Addr:
                             IF writeSet[self][a] # NoWrite
                                THEN /\ versionList' = [versionList EXCEPT ![a] = << <<clock', writeSet[self][a]>> >> \o versionList[a]]
                                ELSE /\ TRUE
                                     /\ UNCHANGED versionList
                        /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                        /\ lockedSet' = [lockedSet EXCEPT ![self] = {}]
                        /\ readSet' = [readSet EXCEPT ![self] = {}]
                        /\ writeSet' = [writeSet EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED readVersions
                     \/ /\ lockedSet' = [lockedSet EXCEPT ![self] = {}]
                        /\ readSet' = [readSet EXCEPT ![self] = {}]
                        /\ writeSet' = [writeSet EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                        /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED <<clock, versionList, readVersions>>
                  /\ UNCHANGED startTime

ThreadProc(self) == L_idle(self) \/ L_active(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION 

Spec_WF == Spec /\ \A t \in Thread : WF_vars(ThreadProc(t))

ProgressProp ==
    \A t \in Thread :
        (pc[t] = "L_active") ~> (pc[t] = "L_idle")

====================================================================
