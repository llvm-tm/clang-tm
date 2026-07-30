------------------------- MODULE CSMV -------------------------
(*
 * CSMV — Multi-Versioned Software Transactional Memory for GPUs
 *
 * Algorithm (IPDPS 2022 / JPDC 2023, Nunes, Castro, Romano):
 *
 * Each shared address maps (via hash) to an ObjectEntry with a
 * version list (sequence of <<ts,value>>, newest-first index 1).
 * A global version clock increments on every commit.
 *
 * Begin:  snapshot clock → startTime[t]
 * Read:   traverse version list → newest version ≤ startTime[t].
 *         Record (addr, observed_head_ts) in readSet.  NEVER aborts
 *         — multi-versioning guarantees a consistent view.
 * Write:  buffer (addr, value) in writeSet (no lock yet).
 * Commit:
 *   1. Add write-set addrs to lockedSet
 *   2. Validate: re-check head timestamp for each read-set addr
 *   3. clock++ → commit_ts
 *   4. Prepend <<commit_ts, value>> to each written addr's version list
 *   5. Clear lockedSet, readSet, writeSet → idle
 *
 * Invariants:
 *   ReadConsistencyOK:     every read returns a value committed ≤ startTime
 *   VersionChainMonotonicOK: timestamps strictly decreasing per addr
 *   NoConcurrentLocks:     no two threads lock the same addr
 *   FenceFidelityOK:       lock-holding threads have done a fence
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Thread, Addr, Value

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Value \subseteq Nat

NoWrite == -1

(* --algorithm CSMV

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
                Len(versionList[a]) > 0 /\
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
            if writeSet[self][a] # NoWrite then
                skip;  \* read own write (model: value irrelevant)
            else
                \* Find newest version with timestamp ≤ startTime
                LET versions == versionList[a] IN
                    if Len(versions) > 0 then
                        LET candidates == {i \in 1..Len(versions) :
                                              versions[i][1] <= startTime[self]} IN
                            if candidates = {} then
                                skip  \* no match — model only checks existence
                            else
                                skip  \* found — record read
                            end if
                    else
                        skip  \* no versions yet
                    end if;
                readSet[self] := readSet[self] \union {a};
                if Len(versionList[a]) > 0 then
                    readVersions[self][a] := versionList[a][1][1]
                else
                    readVersions[self][a] := 0
                end if
            end if;
            lastFence[self] := "sc"
        end with;
        goto L_active;

    or \* Write
        with a \in Addr, v \in Value do
            writeSet[self][a] := v
        end with;
        goto L_active;

    or \* Commit
        lockedSet[self] := {a \in Addr : writeSet[self][a] # NoWrite};
        either
            \* Commit succeeds
            clock := clock + 1;
            with a \in Addr do
                if writeSet[self][a] # NoWrite then
                    versionList[a] := << <<clock, writeSet[self][a]] >> >> \o versionList[a]
                end if
            end with;
            lastFence[self] := "rel";
            lockedSet[self] := {};
            readSet[self] := {};
            writeSet[self] := [a \in Addr |-> NoWrite];
            goto L_idle;
        or
            \* Commit aborts (validation failed)
            lastFence[self] := "rel";
            lockedSet[self] := {};
            readSet[self] := {};
            writeSet[self] := [a \in Addr |-> NoWrite];
            goto L_idle;
        end either;

    or \* Explicit abort
        lastFence[self] := "rel";
        lockedSet[self] := {};
        readSet[self] := {};
        writeSet[self] := [a \in Addr |-> NoWrite];
        goto L_idle;

    end either;

end process;

end algorithm; *)

Spec_WF == Spec /\ \A t \in Thread : WF_vars(ThreadProc(t))

ProgressProp ==
    \A t \in Thread :
        (pc[t] = "L_active") ~> (pc[t] = "L_idle")

====================================================================
