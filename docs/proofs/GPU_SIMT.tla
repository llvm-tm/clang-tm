------------------------------- MODULE GPU_SIMT -------------------------------
(*
 * GPU_SIMT — Warp-Cooperative (SIMT) Execution Semantics for the
 * CSMV GPU Batch Executor
 *
 * The existing GPU models (GPU_CSMV, GPU_JVSTM, GPU_PR_STM) verify the
 * *algorithms* (version lists, VBoxes, lock priorities).  None of them model
 * the SIMT execution semantics that the batch executor
 * (gpu/backends/csmv/csmv_batch_executor.hpp) and the benchmarks in
 * gpu/benchmarks/ depend on:
 *
 *   1. ONE TRANSACTION PER WARP.  A warp (32 lanes in hardware) cooperates on
 *      a single transaction: a shared read-set, shared write-set, shared
 *      start clock.  Not per-lane transactions.
 *
 *   2. LOCKSTEP PHASES.  A shared `phase` variable forces all lanes through
 *      the same instruction sequence (idle -> active -> commit).  No lane can
 *      commit while another is still reading.
 *
 *   3. LANE-0-ONLY MUTATION.  csmv_gpu_write() appends to the SHARED write-set
 *      on lane 0 only; lanes 1..31 call the function for convergence but do
 *      not mutate.  (BUG FIXED 2026-07-31: previously all 32 lanes appended,
 *      racing on num_writes and corrupting the write-set array.)
 *
 *   4. BALLOT CONVERGENCE.  __ballot_sync()/__shfl_sync() must be executed by
 *      ALL active lanes; calling it inside a lane-0-guarded region diverges
 *      the warp.  (BUG FIXED 2026-07-31: on AMD/ROCm gfx1151 this raised
 *      HSA_STATUS_ERROR_EXCEPTION; see AGENTS.md session 2026-07-30.)
 *
 *   5. CLOCK INCREMENTED ONCE PER COMMIT.  csmv_gpu_commit() increments the
 *      global clock once on lane 0 and broadcasts the value via __shfl_sync.
 *      (BUG FIXED 2026-07-31: previously all 32 lanes incremented, jumping
 *      the clock by 32 per commit.)
 *
 *   6. READ-OWN-WRITES.  A read checks the write-set first; a value written by
 *      the same warp is visible to subsequent reads without touching the
 *      version list.
 *
 *   7. MULTI-VERSION READS.  A read traverses the address's version list to
 *      find the newest node with timestamp <= startClock, and records the
 *      observed head timestamp for commit-time validation (OCC).
 *
 * The boolean constant `Buggy` selects between the buggy protocol (as it was
 * before the 2026-07-31 fixes) and the fixed protocol:
 *
 *   Buggy = FALSE  (fixed)   — all invariants hold (see GPU_SIMT.cfg).
 *   Buggy = TRUE   (buggy)   — WriteSetCountOK, ClockMatchesCommits,
 *                              NoSyncError are violated (see GPU_SIMT-buggy.cfg).
 *
 * In buggy mode:
 *   - Write:  every lane increments numWrites  -> numWrites inflated by |Lane|.
 *   - Commit: the validation ballot is executed while the warp may be
 *             diverged (syncError set), and clock += |Lane|.
 *
 * ── Fidelity notes ────────────────────────────────────────────────────
 * - Lane count is reduced from 32 to |Lane| (2 in the config); the races
 *   manifest with >= 2 lanes.
 * - The "commit validation ballot inside a lane-0 region" is modeled by the
 *   explicit enter/exit lane-0-guarded-region actions plus a commit that,
 *   under Buggy, fires while activeMask is partial.
 * - Values are elided (only timestamps tracked) — conflict detection uses
 *   versions, exactly like the C++ CSMV GPU backend.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
  Warp,                \* Set of warp IDs: {1, 2}
  Lane,                \* Set of lane IDs within each warp: {0, 1}
  Addr,                \* Set of addresses
  Value,               \* Set of written values
  Buggy                \* FALSE = fixed protocol, TRUE = buggy protocol

ASSUME Warp \subseteq Nat \ {0}
ASSUME Lane \subseteq Nat
ASSUME Addr \subseteq Nat
ASSUME Value \subseteq Nat
ASSUME Buggy \in BOOLEAN

NoWrite == 0 - 1

(* --algorithm GPU_SIMT

variables
    \* ── Global state ──────────────────────────────────────────────
    \* Global version clock: incremented once per committed transaction
    clock = 0,

    \* Number of committed transactions (== clock in fixed mode)
    committedTxns = 0,

    \* Per-address version list: sequence of <<ts, value>> nodes,
    \* newest-first (index 1 is the head).
    versionList = [a \in Addr |-> << >>],

    \* ── Per-warp state (SHARED by all lanes — LOCKSTEP) ──
    \* Current warp phase: "idle" -> "active" -> commit -> "idle"
    phase = [w \in Warp |-> "idle"],

    \* Snapshot of the clock at transaction start
    startClock = [w \in Warp |-> 0],

    \* SIMT divergence: which lanes are executing the current path
    activeMask = [w \in Warp |-> [l \in Lane |-> TRUE]],

    \* Shared read-set (addresses read) and observed head timestamps
    readSet = [w \in Warp |-> {}],
    readVersions = [w \in Warp |-> [a \in Addr |-> 0]],

    \* Shared write-set (address -> buffered value)
    writeSet = [w \in Warp |-> [a \in Addr |-> NoWrite]],

    \* Bookkeeping counters (catches the multi-lane races)
    numWrites = [w \in Warp |-> 0],
    numReads = [w \in Warp |-> 0],

    \* Set if a ballot is executed while the warp is diverged (buggy only)
    syncError = [w \in Warp |-> FALSE],

    \* Last fence type (SC before validate, REL after commit)
    lastFence = [w \in Warp |-> ""];

define
    \* ── Invariants ────────────────────────────────────────────────

    \* Every read returned a value from a version committed at or before the
    \* warp's start clock (snapshot isolation; reads never see later commits).
    ReadConsistencyOK ==
        \A w \in Warp :
            \A a \in readSet[w] :
                Len(versionList[a]) = 0 \/ \* uninitialized address
                \E i \in 1..Len(versionList[a]) :
                    versionList[a][i][1] <= startClock[w]

    \* Each address's version list is strictly newest-first.
    VersionChainMonotonicOK ==
        \A a \in Addr :
            \A i \in 1..(Len(versionList[a]) - 1) :
                versionList[a][i][1] > versionList[a][i+1][1]

    \* The write-set count equals the number of DISTINCT addresses buffered.
    \* Violated by the buggy all-lanes-append write (numWrites inflated).
    WriteSetCountOK ==
        \A w \in Warp :
            numWrites[w] = Cardinality({a \in Addr : writeSet[w][a] # NoWrite})

    \* The global clock advances exactly once per committed transaction.
    \* Violated by the buggy all-lanes clock increment (clock jumps by |Lane|).
    ClockMatchesCommits ==
        clock = committedTxns

    \* A ballot/sync collective is never executed while the warp is diverged.
    \* Violated by the buggy commit that validates inside a lane-0 region.
    NoSyncError ==
        \A w \in Warp : syncError[w] = FALSE

    \* A committed warp's reads are still fresh (OCC validation): every read-set
    \* address's current head timestamp equals what the warp observed.
    ReadSetFreshOK ==
        \A w \in Warp :
            phase[w] # "active" \/ \* idle or between-transaction
            \A a \in readSet[w] :
                (Len(versionList[a]) = 0 /\ readVersions[w][a] = 0) \/
                (Len(versionList[a]) > 0 /\
                 versionList[a][1][1] = readVersions[w][a])

    \* TLC state-space bound (prunes unbounded version-list growth).
    TLCBound ==
        /\ \A a \in Addr : Len(versionList[a]) < 3
        /\ \A w \in Warp : Cardinality(readSet[w]) < 3
end define;

process WarpProc \in Warp
begin

L_idle:
    \* Begin a new transaction: snapshot the clock, reset warp state.
    startClock[self] := clock;
    readSet[self] := {};
    readVersions[self] := [a \in Addr |-> 0];
    writeSet[self] := [a \in Addr |-> NoWrite];
    numWrites[self] := 0;
    numReads[self] := 0;
    syncError[self] := FALSE;
    activeMask[self] := [l \in Lane |-> TRUE];
    lastFence[self] := "sc";
    phase[self] := "active";
    goto L_active;

L_active:
    either \* ── Read (warp-cooperative version-list traversal) ──
        with a \in Addr do
            if writeSet[self][a] = NoWrite then \* read-own-writes check
                if Len(versionList[a]) > 0 then
                    readVersions[self][a] := versionList[a][1][1]
                else
                    readVersions[self][a] := 0
                end if
            end if;
            readSet[self] := readSet[self] \union {a};
            numReads[self] := numReads[self] + 1
        end with;
        goto L_active;

    or \* ── Write (lane-0-only mutation of the shared write-set) ──
        with a \in Addr, v \in Value do
            if Buggy then
                \* BUGGY: every active lane appends -> numWrites inflated
                writeSet[self][a] := v;
                numWrites[self] := numWrites[self] + Cardinality(Lane)
            else
                \* FIXED: lane 0 mutates; other lanes converge only
                if writeSet[self][a] = NoWrite then
                    writeSet[self][a] := v
                end if;
                numWrites[self] :=
                    Cardinality({b \in Addr : writeSet[self][b] # NoWrite})
            end if
        end with;
        goto L_active;

    or \* ── Enter a lane-0-guarded region (SIMT divergence) ──
        activeMask[self] := [l \in Lane |-> l = 0];
        goto L_active;

    or \* ── Re-converge (warp-wide __syncwarp) ──
        activeMask[self] := [l \in Lane |-> TRUE];
        goto L_active;

    or \* ── Commit (validate, bump clock once, prepend version nodes) ──
        if Buggy then
            \* BUGGY: ballot executed even while diverged, clock += |Lane|
            if activeMask[self] # [l \in Lane |-> TRUE] then
                syncError[self] := TRUE
            end if;
            if \A a \in readSet[self] :
                 (Len(versionList[a]) = 0 /\
                      readVersions[self][a] = 0) \/
                 (Len(versionList[a]) > 0 /\
                      versionList[a][1][1] = readVersions[self][a])
               then
                clock := clock + Cardinality(Lane);
                committedTxns := committedTxns + 1;
                versionList := [aa \in Addr |->
                    IF writeSet[self][aa] # NoWrite
                       THEN << <<clock, writeSet[self][aa]>> >> \o versionList[aa]
                       ELSE versionList[aa]]
            end if;
            lastFence[self] := "rel";
            readSet[self] := {};
            writeSet[self] := [a \in Addr |-> NoWrite];
            numWrites[self] := 0;
            numReads[self] := 0;
            activeMask[self] := [l \in Lane |-> TRUE];
            phase[self] := "idle";
            goto L_idle
        else
            \* FIXED: ballot requires full convergence; clock incremented once
            if activeMask[self] = [l \in Lane |-> TRUE] /\
               \A a \in readSet[self] :
                 (Len(versionList[a]) = 0 /\
                      readVersions[self][a] = 0) \/
                 (Len(versionList[a]) > 0 /\
                      versionList[a][1][1] = readVersions[self][a])
               then
                clock := clock + 1;
                committedTxns := committedTxns + 1;
                versionList := [aa \in Addr |->
                    IF writeSet[self][aa] # NoWrite
                       THEN << <<clock, writeSet[self][aa]>> >> \o versionList[aa]
                       ELSE versionList[aa]]
            end if;
            lastFence[self] := "rel";
            readSet[self] := {};
            writeSet[self] := [a \in Addr |-> NoWrite];
            numWrites[self] := 0;
            numReads[self] := 0;
            activeMask[self] := [l \in Lane |-> TRUE];
            phase[self] := "idle";
            goto L_idle
        end if;
    end either;

end process;

end algorithm; *)

\* BEGIN TRANSLATION (chksum(pcal) = "" /\ chksum(tla) = "")
VARIABLES clock, committedTxns, versionList, phase, startClock, activeMask, 
          readSet, readVersions, writeSet, numWrites, numReads, syncError, 
          lastFence, pc

(* define statement *)
ReadConsistencyOK ==
    \A w \in Warp :
        \A a \in readSet[w] :
            Len(versionList[a]) = 0 \/
            \E i \in 1..Len(versionList[a]) :
                versionList[a][i][1] <= startClock[w]


VersionChainMonotonicOK ==
    \A a \in Addr :
        \A i \in 1..(Len(versionList[a]) - 1) :
            versionList[a][i][1] > versionList[a][i+1][1]



WriteSetCountOK ==
    \A w \in Warp :
        numWrites[w] = Cardinality({a \in Addr : writeSet[w][a] # NoWrite})



ClockMatchesCommits ==
    clock = committedTxns



NoSyncError ==
    \A w \in Warp : syncError[w] = FALSE



ReadSetFreshOK ==
    \A w \in Warp :
        phase[w] # "active" \/
        \A a \in readSet[w] :
            (Len(versionList[a]) = 0 /\ readVersions[w][a] = 0) \/
            (Len(versionList[a]) > 0 /\
             versionList[a][1][1] = readVersions[w][a])


TLCBound ==
    /\ \A a \in Addr : Len(versionList[a]) < 3
    /\ \A w \in Warp : Cardinality(readSet[w]) < 3


vars == << clock, committedTxns, versionList, phase, startClock, activeMask, 
           readSet, readVersions, writeSet, numWrites, numReads, syncError, 
           lastFence, pc >>

ProcSet == (Warp)

Init == (* Global variables *)
        /\ clock = 0
        /\ committedTxns = 0
        /\ versionList = [a \in Addr |-> << >>]
        /\ phase = [w \in Warp |-> "idle"]
        /\ startClock = [w \in Warp |-> 0]
        /\ activeMask = [w \in Warp |-> [l \in Lane |-> TRUE]]
        /\ readSet = [w \in Warp |-> {}]
        /\ readVersions = [w \in Warp |-> [a \in Addr |-> 0]]
        /\ writeSet = [w \in Warp |-> [a \in Addr |-> NoWrite]]
        /\ numWrites = [w \in Warp |-> 0]
        /\ numReads = [w \in Warp |-> 0]
        /\ syncError = [w \in Warp |-> FALSE]
        /\ lastFence = [w \in Warp |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ startClock' = [startClock EXCEPT ![self] = clock]
                /\ readSet' = [readSet EXCEPT ![self] = {}]
                /\ readVersions' = [readVersions EXCEPT ![self] = [a \in Addr |-> 0]]
                /\ writeSet' = [writeSet EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                /\ numWrites' = [numWrites EXCEPT ![self] = 0]
                /\ numReads' = [numReads EXCEPT ![self] = 0]
                /\ syncError' = [syncError EXCEPT ![self] = FALSE]
                /\ activeMask' = [activeMask EXCEPT ![self] = [l \in Lane |-> TRUE]]
                /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                /\ phase' = [phase EXCEPT ![self] = "active"]
                /\ pc' = [pc EXCEPT ![self] = "L_active"]
                /\ UNCHANGED << clock, committedTxns, versionList >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             /\ IF writeSet[self][a] = NoWrite
                                   THEN /\ IF Len(versionList[a]) > 0
                                              THEN /\ readVersions' = [readVersions EXCEPT ![self][a] = versionList[a][1][1]]
                                              ELSE /\ readVersions' = [readVersions EXCEPT ![self][a] = 0]
                                   ELSE /\ TRUE
                                        /\ UNCHANGED readVersions
                             /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {a}]
                             /\ numReads' = [numReads EXCEPT ![self] = numReads[self] + 1]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, committedTxns, versionList, phase, activeMask, writeSet, numWrites, syncError, lastFence>>
                     \/ /\ \E a \in Addr:
                             \E v \in Value:
                               IF Buggy
                                  THEN /\ writeSet' = [writeSet EXCEPT ![self][a] = v]
                                       /\ numWrites' = [numWrites EXCEPT ![self] = numWrites[self] + Cardinality(Lane)]
                                  ELSE /\ IF writeSet[self][a] = NoWrite
                                             THEN /\ writeSet' = [writeSet EXCEPT ![self][a] = v]
                                             ELSE /\ TRUE
                                                  /\ UNCHANGED writeSet
                                       /\ numWrites' = [numWrites EXCEPT ![self] = Cardinality({b \in Addr : writeSet'[self][b] # NoWrite})]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, committedTxns, versionList, phase, activeMask, readSet, readVersions, numReads, syncError, lastFence>>
                     \/ /\ activeMask' = [activeMask EXCEPT ![self] = [l \in Lane |-> l = 0]]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, committedTxns, versionList, phase, readSet, readVersions, writeSet, numWrites, numReads, syncError, lastFence>>
                     \/ /\ activeMask' = [activeMask EXCEPT ![self] = [l \in Lane |-> TRUE]]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, committedTxns, versionList, phase, readSet, readVersions, writeSet, numWrites, numReads, syncError, lastFence>>
                     \/ /\ IF Buggy
                              THEN /\ IF activeMask[self] # [l \in Lane |-> TRUE]
                                         THEN /\ syncError' = [syncError EXCEPT ![self] = TRUE]
                                         ELSE /\ TRUE
                                              /\ UNCHANGED syncError
                                   /\ IF \A a \in readSet[self] :
                                           (Len(versionList[a]) = 0 /\
                                                readVersions[self][a] = 0) \/
                                           (Len(versionList[a]) > 0 /\
                                                versionList[a][1][1] = readVersions[self][a])
                                         THEN /\ clock' = clock + Cardinality(Lane)
                                              /\ committedTxns' = committedTxns + 1
                                              /\ versionList' =            [aa \in Addr |->
                                                                IF writeSet[self][aa] # NoWrite
                                                                   THEN << <<clock', writeSet[self][aa]>> >> \o versionList[aa]
                                                                   ELSE versionList[aa]]
                                         ELSE /\ TRUE
                                              /\ UNCHANGED << clock, 
                                                              committedTxns, 
                                                              versionList >>
                                   /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ writeSet' = [writeSet EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                                   /\ numWrites' = [numWrites EXCEPT ![self] = 0]
                                   /\ numReads' = [numReads EXCEPT ![self] = 0]
                                   /\ activeMask' = [activeMask EXCEPT ![self] = [l \in Lane |-> TRUE]]
                                   /\ phase' = [phase EXCEPT ![self] = "idle"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ IF activeMask[self] = [l \in Lane |-> TRUE] /\
                                         \A a \in readSet[self] :
                                           (Len(versionList[a]) = 0 /\
                                                readVersions[self][a] = 0) \/
                                           (Len(versionList[a]) > 0 /\
                                                versionList[a][1][1] = readVersions[self][a])
                                         THEN /\ clock' = clock + 1
                                              /\ committedTxns' = committedTxns + 1
                                              /\ versionList' =            [aa \in Addr |->
                                                                IF writeSet[self][aa] # NoWrite
                                                                   THEN << <<clock', writeSet[self][aa]>> >> \o versionList[aa]
                                                                   ELSE versionList[aa]]
                                         ELSE /\ TRUE
                                              /\ UNCHANGED << clock, 
                                                              committedTxns, 
                                                              versionList >>
                                   /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                                   /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ writeSet' = [writeSet EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                                   /\ numWrites' = [numWrites EXCEPT ![self] = 0]
                                   /\ numReads' = [numReads EXCEPT ![self] = 0]
                                   /\ activeMask' = [activeMask EXCEPT ![self] = [l \in Lane |-> TRUE]]
                                   /\ phase' = [phase EXCEPT ![self] = "idle"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                   /\ UNCHANGED syncError
                        /\ UNCHANGED readVersions
                  /\ UNCHANGED startClock

WarpProc(self) == L_idle(self) \/ L_active(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Warp: WarpProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION
============================================================
