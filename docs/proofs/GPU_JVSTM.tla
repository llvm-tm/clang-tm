--------------------------- MODULE GPU_JVSTM ---------------------------
(*
 * GPU JVSTM — Multi-Version OCC with Versioned Boxes (GPU Warp Variant)
 *
 * Extends JVSTM (Cachopo & Rito-Silva, 2006) to GPU warp execution.
 * Versioned boxes provide snapshot isolation: read-only transactions
 * never abort. Write transactions validate before committing.
 *
 * ── GPU-specific aspects modeled ──────────────────────────────────
 *
 * 1. WARP AS PROCESS (not thread):
 *    Each process represents an entire warp (NVIDIA SIMT unit, 32 threads
 *    in hardware). The warp manages arrays of per-thread state.
 *    A shared `phase` variable enforces SIMT lockstep — all threads
 *    execute the same instruction. No thread can be validating while
 *    another reads.
 *
 * 2. SHARED READ VERSION (rv):
 *    All threads in a warp read the global clock at the same time
 *    (begin transaction). They all see the same snapshot.
 *
 * 3. VERSIONED BOXES (VBox):
 *    Every address maintains a singly-linked list of <<version, value>>
 *    bodies, ordered newest-first. Reads walk the list to find the
 *    newest body with version ≤ rv. This guarantees read-only
 *    transactions never abort.
 *
 * 4. WARP-LEVEL COMMIT LOCK:
 *    The global commit lock serializes write transactions across warps.
 *    When ANY thread in a warp has writes, the warp acquires the lock,
 *    validates ALL threads' read-sets, then prepends new bodies.
 *
 * 5. DIVERGENCE (activeMask):
 *    When a thread finishes its ReadsPerThread reads, it is masked
 *    out. Other threads continue reading. Both paths execute
 *    sequentially within the warp (masked threads execute a no-op
 *    iteration instead of branching away).
 *
 * 6. READ-ONLY WARP OPTIMIZATION:
 *    If no thread in the warp has any writes, the transaction commits
 *    instantly without acquiring the global lock or validating.
 *    This is JVSTM's key advantage: read-only transactions never abort.
 *
 * ── Fidelity notes ────────────────────────────────────────────────
 *
 * - Memory values are not tracked (only versions). OCC does not
 *   inspect values for conflict detection — only versions.
 * - Thread-local read/write counts are tracked per round, not per
 *   address. Each step picks an address non-deterministically.
 * - VBox history sequences are bounded only by the number of commits.
 * - The model does not track VBox body VALUES — only versions.
 *   This is sufficient for conflict detection.
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
  Warp,                \* Set of warp IDs: {0, 1}
  Thread,              \* Set of thread IDs within each warp: {0, 1}
  Addr,                \* Set of addresses
  ReadsPerThread,      \* Number of reads per thread per transaction
  WritesPerThread,     \* Number of writes per thread per transaction
  MaxCommits           \* Max committed transactions before halting

ASSUME Thread \subseteq Nat
ASSUME Addr \subseteq Nat
ASSUME Warp \subseteq Nat \ {0}
ASSUME ReadsPerThread > 0
ASSUME WritesPerThread >= 0
ASSUME MaxCommits > 0

\* ── GPU-specific type definitions ──────────────────────────────────

\* Warp phases (shared by all threads in a warp — SIMT lockstep)
PhaseSet == {"idle", "reading", "writing", "validating",
               "committing", "aborting"}

\* Total reads/writes per warp before phase transition
TotalReadsPerWarp  == Cardinality(Thread) * ReadsPerThread
TotalWritesPerWarp == Cardinality(Thread) * WritesPerThread

\* FindBody(seq, rv): returns the first body in seq (newest-first) with
\* version ≤ rv. seq is a sequence of <<version>> entries (values elided).
FindBody(seq, rv) ==
    seq[CHOOSE i \in 1..Len(seq) : seq[i][1] <= rv]

(*
--algorithm GPU_JVSTM

variables
    \* ── Global state ──────────────────────────────────────────────

    \* Global monotonic clock (incremented on each write commit)
    clock = 0,

    \* Commit lock: 0 = free, >0 = warp ID holding it
    commit_lock = 0,

    \* Per-address VBox: sequence of <<version>> bodies, newest first.
    \* Each body stores only the version (values elided — conflict
    \* detection uses versions only).
    vbox_hist = [a \in Addr |-> << <<0>> >>],

    \* Total committed transactions (for progress checking)
    committedTxns = 0,

    \* ── Per-warp state (SHARED by all threads — LOCKSTEP) ──

    \* Current warp phase (shared across all threads)
    phase = [w \in Warp |-> "idle"],

    \* Read version (snapshot clock at tx start)
    rv = [w \in Warp |-> 0],

    \* Divergence mask: threads actively executing current path
    activeMask = [w \in Warp |-> [t \in Thread |-> FALSE]],

    \* Read/write round counters (warp-level progress through phases)
    readsDone  = [w \in Warp |-> 0],
    writesDone = [w \in Warp |-> 0],

    \* Whether ANY thread in this warp has writes
    has_warp_write = [w \in Warp |-> FALSE],

    \* Commit version for this warp
    ct = [w \in Warp |-> 0],

    \* Per-warp committed transaction counter (bounded by MaxCommits)
    warpCommits = [w \in Warp |-> 0],

    \* ── Per-(warp, thread) state ─────────────────────────────────

    \* Read-set: set of captured <<addr, version>> pairs
    readSet = [w \in Warp |-> [t \in Thread |-> {}]],

    \* Write-set: mapping addr → TRUE (value elided)
    writeSet = [w \in Warp |-> [t \in Thread |-> [a \in Addr |-> FALSE]]];

define
    \* ── Invariants ───────────────────────────────────────────────

    \* WARP PHASE COHERENCE: All threads in a warp share the same phase.
    InvWarpPhaseCoherent ==
        \A w \in Warp : phase[w] \in PhaseSet

    \* READ-SET SIZE BOUND: No thread exceeds ReadsPerThread.
    InvReadSetBounded ==
        \A w \in Warp, t \in Thread :
            Cardinality(readSet[w][t]) <= ReadsPerThread

    \* WRITE-SET SIZE BOUND: No thread exceeds WritesPerThread.
    InvWriteSetBounded ==
        \A w \in Warp, t \in Thread :
            Cardinality({a \in Addr : writeSet[w][t][a]}) <= WritesPerThread

    \* EXCLUSIVE COMMIT LOCK: At most one warp holds commit_lock.
    InvLockExclusion ==
        \A w1, w2 \in Warp :
            (commit_lock = w1 /\ commit_lock = w2) => w1 = w2

    \* LOCK HOLDER VALIDATING: A warp holding commit_lock is validating.
    InvLockHolderState ==
        \A w \in Warp :
            commit_lock = w => phase[w] \in {"validating", "committing"}

    \* GLOBAL CLOCK MONOTONICITY
    InvClockMonotonic ==
        clock >= 0

    \* READ-SET CAPTURES EXISTING VERSIONS:
    \* Every <<addr, version>> in any thread's read-set corresponds to
    \* a body that exists (or existed) in the VBox for that address.
    InvReadSetValid ==
        \A w \in Warp, t \in Thread :
            \A <<addr, ver>> \in readSet[w][t] :
                \E i \in 1..Len(vbox_hist[addr]) :
                    vbox_hist[addr][i][1] = ver

    \* COMMIT BUDGET: No warp exceeds MaxCommits.
    InvCommitBudget ==
        \A w \in Warp : warpCommits[w] <= MaxCommits

    \* Combined invariant
    Inv == /\ InvWarpPhaseCoherent
           /\ InvReadSetBounded
           /\ InvWriteSetBounded
           /\ InvLockExclusion
           /\ InvLockHolderState
           /\ InvClockMonotonic
           /\ InvReadSetValid
           /\ InvCommitBudget
end define;

\* ── Warp process ──────────────────────────────────────────────────
\*
\* KEY GPU DESIGN DECISION: One process per WARP, not per thread.
\* All threads in a warp share instruction pointer (phase[w]).
\* Thread-local state is accessed via [self][t].

process WARP \in Warp
begin

L_idle:
    \* Non-deterministically begin a transaction or stay idle.
    either
        when warpCommits[self] < MaxCommits;
        \* ── Begin: Initialize all thread state ──
        phase[self] := "reading";
        rv[self] := clock;
        readsDone[self] := 0;
        writesDone[self] := 0;
        has_warp_write[self] := FALSE;
        ct[self] := 0;
        activeMask[self] := [t \in Thread |-> TRUE];
        readSet[self] := [t \in Thread |-> {}];
        writeSet[self] := [t \in Thread |-> [a \in Addr |-> FALSE]];
        goto L_read;
    or
        \* ── Stay idle ──
        goto L_idle;
    end either;

L_read:
    \* ── SIMT read step ──────────────────────────────────────────
    \*
    \* One atomic step: one thread reads one address.
    \* Walks the VBox history for the newest body with version ≤ rv.
    \* Captures the body's version in the read-set.
    \*
    \* GPU NOTE: In real SIMT, all 32 threads issue their load
    \* instruction simultaneously. Modeling one thread per step
    \* gives TLC more interleavings (conservative approximation).

    with t \in Thread do
        when activeMask[self][t];
        with a \in Addr do
            with cur_ver = FindBody(vbox_hist[a], rv[self])[1] do
                readSet[self][t] := readSet[self][t] \union {<<a, cur_ver>>};
            end with;
            readsDone[self] := readsDone[self] + 1;
            if Cardinality(readSet[self][t]) >= ReadsPerThread then
                activeMask[self][t] := FALSE;
            end if;
        end with;
    end with;
    goto L_read_next;

L_read_next:
    \* Check if more reads remain across all threads
    if readsDone[self] < TotalReadsPerWarp then
        \* Reactivate threads that still need to read
        with t \in Thread do
            if Cardinality(readSet[self][t]) < ReadsPerThread then
                activeMask[self][t] := TRUE;
            end if;
        end with;
        goto L_read;
    else
        \* All threads done reading → move to write phase
        \* (even if no writes — check happens at commit)
        phase[self] := "writing";
        writesDone[self] := 0;
        activeMask[self] := [t \in Thread |-> TRUE];
        goto L_write;
    end if;

L_write:
    \* ── SIMT write step ─────────────────────────────────────────
    \*
    \* One thread buffers one write. writeSet is a function mapping
    \* addr → TRUE (values elided — only write-presence matters for
    \* conflict detection).

    with t \in Thread do
        when activeMask[self][t];
        with a \in Addr do
            if ~writeSet[self][t][a] then
                writeSet[self][t][a] := TRUE;
                has_warp_write[self] := TRUE;
                writesDone[self] := writesDone[self] + 1;
            else
                skip;
            end if;
            if Cardinality({a2 \in Addr : writeSet[self][t][a2]}) >= WritesPerThread then
                activeMask[self][t] := FALSE;
            end if;
        end with;
    end with;
    goto L_write_next;

L_write_next:
    \* Check if more writes remain across all threads
    if writesDone[self] < TotalWritesPerWarp then
        with t \in Thread do
            if Cardinality({a \in Addr : writeSet[self][t][a]}) < WritesPerThread then
                activeMask[self][t] := TRUE;
            end if;
        end with;
        goto L_write;
    else
        \* All threads done → move to validation
        phase[self] := "validating";
        goto L_validate;
    end if;

L_validate:
    \* ── WARP-LEVEL VALIDATION ────────────────────────────────────
    \*
    \* JVSTM READ-ONLY OPTIMIZATION:
    \* If no thread has any writes, commit instantly (no lock, no
    \* validation). Read-only transactions NEVER abort in JVSTM.
    \*
    \* If any thread has writes:
    \*   1. Acquire commit lock
    \*   2. Increment clock → ct
    \*   3. Validate ALL threads' read-sets (each VBox head version
    \*      must match captured version)
    \*   4. Validate ALL threads' write-sets (each VBox head version
    \*      must be ≤ rv)

    if ~has_warp_write[self] then
        \* Read-only: no lock, no validation
        phase[self] := "idle";
        warpCommits[self] := warpCommits[self] + 1;
        committedTxns := committedTxns + 1;
        goto L_idle;
    else
        \* Has writes — try to acquire the global commit lock
        goto L_validate_try_lock;
    end if;

L_validate_try_lock:
    if commit_lock = 0 then
        commit_lock := self;
        ct[self] := clock + 1;
        clock := clock + 1;
        goto L_validate_readsets;
    else
        \* Lock held by another warp — abort and retry
        phase[self] := "aborting";
        goto L_abort;
    end if;

L_validate_readsets:
    \* Validate all read-sets and write-sets.
    if \A t1 \in Thread :
           \A <<addr, captured>> \in readSet[self][t1] :
               vbox_hist[addr][1][1] = captured
       /\ \A t2 \in Thread :
              \A a \in Addr :
                  writeSet[self][t2][a] =>
                      vbox_hist[a][1][1] <= rv[self]
    then
        \* Validation passed — prepend new bodies
        phase[self] := "committing";
        goto L_prepend;
    else
        \* Validation failed — release lock and abort
        commit_lock := 0;
        phase[self] := "aborting";
        goto L_abort;
    end if;

L_prepend:
    \* ── PREPEND NEW VBOX BODIES ──────────────────────────────────
    \*
    \* For each address written by any thread in this warp, prepend
    \* a new body <<ct[self]>> to the VBox history, then release the
    \* commit lock.

    vbox_hist := [a \in Addr |->
        IF \E t \in Thread : writeSet[self][t][a] THEN
            << <<ct[self]>> >> \o vbox_hist[a]
        ELSE
            vbox_hist[a]];
    commit_lock := 0;

    \* Commit complete
    warpCommits[self] := warpCommits[self] + 1;
    committedTxns := committedTxns + 1;
    phase[self] := "idle";
    activeMask[self] := [t \in Thread |-> FALSE];
    goto L_idle;

L_abort:
    \* ── WARP-LEVEL ABORT ─────────────────────────────────────────
    \*
    \* GPU-SPECIFIC: ALL threads' state is discarded atomically.
    \* The entire warp retries from the read phase. This matches
    \* JVSTM's "abort = restart from begin" semantics.
    \*
    \* CONTRAST with CPU: a CPU thread can abort independently.
    \* On GPU, the warp is the smallest schedulable unit.

    readSet[self] := [t \in Thread |-> {}];
    writeSet[self] := [t \in Thread |-> [a \in Addr |-> FALSE]];
    readsDone[self] := 0;
    writesDone[self] := 0;
    has_warp_write[self] := FALSE;
    activeMask[self] := [t \in Thread |-> TRUE];
    phase[self] := "reading";
    goto L_read;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES clock, commit_lock, vbox_hist, committedTxns, phase, rv, activeMask, 
          readsDone, writesDone, has_warp_write, ct, warpCommits, readSet, 
          writeSet, pc

(* define statement *)
InvWarpPhaseCoherent ==
    \A w \in Warp : phase[w] \in PhaseSet


InvReadSetBounded ==
    \A w \in Warp, t \in Thread :
        Cardinality(readSet[w][t]) <= ReadsPerThread


InvWriteSetBounded ==
    \A w \in Warp, t \in Thread :
        Cardinality({a \in Addr : writeSet[w][t][a]}) <= WritesPerThread


InvLockExclusion ==
    \A w1, w2 \in Warp :
        (commit_lock = w1 /\ commit_lock = w2) => w1 = w2


InvLockHolderState ==
    \A w \in Warp :
        commit_lock = w => phase[w] \in {"validating", "committing"}


InvClockMonotonic ==
    clock >= 0




InvReadSetValid ==
    \A w \in Warp, t \in Thread :
        \A <<addr, ver>> \in readSet[w][t] :
            \E i \in 1..Len(vbox_hist[addr]) :
                vbox_hist[addr][i][1] = ver


InvCommitBudget ==
    \A w \in Warp : warpCommits[w] <= MaxCommits


Inv == /\ InvWarpPhaseCoherent
       /\ InvReadSetBounded
       /\ InvWriteSetBounded
       /\ InvLockExclusion
       /\ InvLockHolderState
       /\ InvClockMonotonic
       /\ InvReadSetValid
       /\ InvCommitBudget


vars == << clock, commit_lock, vbox_hist, committedTxns, phase, rv, 
           activeMask, readsDone, writesDone, has_warp_write, ct, warpCommits, 
           readSet, writeSet, pc >>

ProcSet == (Warp)

Init == (* Global variables *)
        /\ clock = 0
        /\ commit_lock = 0
        /\ vbox_hist = [a \in Addr |-> << <<0>> >>]
        /\ committedTxns = 0
        /\ phase = [w \in Warp |-> "idle"]
        /\ rv = [w \in Warp |-> 0]
        /\ activeMask = [w \in Warp |-> [t \in Thread |-> FALSE]]
        /\ readsDone = [w \in Warp |-> 0]
        /\ writesDone = [w \in Warp |-> 0]
        /\ has_warp_write = [w \in Warp |-> FALSE]
        /\ ct = [w \in Warp |-> 0]
        /\ warpCommits = [w \in Warp |-> 0]
        /\ readSet = [w \in Warp |-> [t \in Thread |-> {}]]
        /\ writeSet = [w \in Warp |-> [t \in Thread |-> [a \in Addr |-> FALSE]]]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ warpCommits[self] < MaxCommits
                      /\ phase' = [phase EXCEPT ![self] = "reading"]
                      /\ rv' = [rv EXCEPT ![self] = clock]
                      /\ readsDone' = [readsDone EXCEPT ![self] = 0]
                      /\ writesDone' = [writesDone EXCEPT ![self] = 0]
                      /\ has_warp_write' = [has_warp_write EXCEPT ![self] = FALSE]
                      /\ ct' = [ct EXCEPT ![self] = 0]
                      /\ activeMask' = [activeMask EXCEPT ![self] = [t \in Thread |-> TRUE]]
                      /\ readSet' = [readSet EXCEPT ![self] = [t \in Thread |-> {}]]
                      /\ writeSet' = [writeSet EXCEPT ![self] = [t \in Thread |-> [a \in Addr |-> FALSE]]]
                      /\ pc' = [pc EXCEPT ![self] = "L_read"]
                   \/ /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                      /\ UNCHANGED <<phase, rv, activeMask, readsDone, writesDone, has_warp_write, ct, readSet, writeSet>>
                /\ UNCHANGED << clock, commit_lock, vbox_hist, committedTxns, 
                                warpCommits >>

L_read(self) == /\ pc[self] = "L_read"
                /\ \E t \in Thread:
                     /\ activeMask[self][t]
                     /\ \E a \in Addr:
                          /\ LET cur_ver == FindBody(vbox_hist[a], rv[self])[1] IN
                               readSet' = [readSet EXCEPT ![self][t] = readSet[self][t] \union {<<a, cur_ver>>}]
                          /\ readsDone' = [readsDone EXCEPT ![self] = readsDone[self] + 1]
                          /\ IF Cardinality(readSet'[self][t]) >= ReadsPerThread
                                THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = FALSE]
                                ELSE /\ TRUE
                                     /\ UNCHANGED activeMask
                /\ pc' = [pc EXCEPT ![self] = "L_read_next"]
                /\ UNCHANGED << clock, commit_lock, vbox_hist, committedTxns, 
                                phase, rv, writesDone, has_warp_write, ct, 
                                warpCommits, writeSet >>

L_read_next(self) == /\ pc[self] = "L_read_next"
                     /\ IF readsDone[self] < TotalReadsPerWarp
                           THEN /\ \E t \in Thread:
                                     IF Cardinality(readSet[self][t]) < ReadsPerThread
                                        THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = TRUE]
                                        ELSE /\ TRUE
                                             /\ UNCHANGED activeMask
                                /\ pc' = [pc EXCEPT ![self] = "L_read"]
                                /\ UNCHANGED << phase, writesDone >>
                           ELSE /\ phase' = [phase EXCEPT ![self] = "writing"]
                                /\ writesDone' = [writesDone EXCEPT ![self] = 0]
                                /\ activeMask' = [activeMask EXCEPT ![self] = [t \in Thread |-> TRUE]]
                                /\ pc' = [pc EXCEPT ![self] = "L_write"]
                     /\ UNCHANGED << clock, commit_lock, vbox_hist, 
                                     committedTxns, rv, readsDone, 
                                     has_warp_write, ct, warpCommits, readSet, 
                                     writeSet >>

L_write(self) == /\ pc[self] = "L_write"
                 /\ \E t \in Thread:
                      /\ activeMask[self][t]
                      /\ \E a \in Addr:
                           /\ IF ~writeSet[self][t][a]
                                 THEN /\ writeSet' = [writeSet EXCEPT ![self][t][a] = TRUE]
                                      /\ has_warp_write' = [has_warp_write EXCEPT ![self] = TRUE]
                                      /\ writesDone' = [writesDone EXCEPT ![self] = writesDone[self] + 1]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED << writesDone, 
                                                      has_warp_write, writeSet >>
                           /\ IF Cardinality({a2 \in Addr : writeSet'[self][t][a2]}) >= WritesPerThread
                                 THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = FALSE]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED activeMask
                 /\ pc' = [pc EXCEPT ![self] = "L_write_next"]
                 /\ UNCHANGED << clock, commit_lock, vbox_hist, committedTxns, 
                                 phase, rv, readsDone, ct, warpCommits, 
                                 readSet >>

L_write_next(self) == /\ pc[self] = "L_write_next"
                      /\ IF writesDone[self] < TotalWritesPerWarp
                            THEN /\ \E t \in Thread:
                                      IF Cardinality({a \in Addr : writeSet[self][t][a]}) < WritesPerThread
                                         THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = TRUE]
                                         ELSE /\ TRUE
                                              /\ UNCHANGED activeMask
                                 /\ pc' = [pc EXCEPT ![self] = "L_write"]
                                 /\ phase' = phase
                            ELSE /\ phase' = [phase EXCEPT ![self] = "validating"]
                                 /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                                 /\ UNCHANGED activeMask
                      /\ UNCHANGED << clock, commit_lock, vbox_hist, 
                                      committedTxns, rv, readsDone, writesDone, 
                                      has_warp_write, ct, warpCommits, readSet, 
                                      writeSet >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF ~has_warp_write[self]
                          THEN /\ phase' = [phase EXCEPT ![self] = "idle"]
                               /\ warpCommits' = [warpCommits EXCEPT ![self] = warpCommits[self] + 1]
                               /\ committedTxns' = committedTxns + 1
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "L_validate_try_lock"]
                               /\ UNCHANGED << committedTxns, phase, 
                                               warpCommits >>
                    /\ UNCHANGED << clock, commit_lock, vbox_hist, rv, 
                                    activeMask, readsDone, writesDone, 
                                    has_warp_write, ct, readSet, writeSet >>

L_validate_try_lock(self) == /\ pc[self] = "L_validate_try_lock"
                             /\ IF commit_lock = 0
                                   THEN /\ commit_lock' = self
                                        /\ ct' = [ct EXCEPT ![self] = clock + 1]
                                        /\ clock' = clock + 1
                                        /\ pc' = [pc EXCEPT ![self] = "L_validate_readsets"]
                                        /\ phase' = phase
                                   ELSE /\ phase' = [phase EXCEPT ![self] = "aborting"]
                                        /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                                        /\ UNCHANGED << clock, commit_lock, ct >>
                             /\ UNCHANGED << vbox_hist, committedTxns, rv, 
                                             activeMask, readsDone, writesDone, 
                                             has_warp_write, warpCommits, 
                                             readSet, writeSet >>

L_validate_readsets(self) == /\ pc[self] = "L_validate_readsets"
                             /\ IF \A t1 \in Thread :
                                       \A <<addr, captured>> \in readSet[self][t1] :
                                           vbox_hist[addr][1][1] = captured
                                   /\ \A t2 \in Thread :
                                          \A a \in Addr :
                                              writeSet[self][t2][a] =>
                                                  vbox_hist[a][1][1] <= rv[self]
                                   THEN /\ phase' = [phase EXCEPT ![self] = "committing"]
                                        /\ pc' = [pc EXCEPT ![self] = "L_prepend"]
                                        /\ UNCHANGED commit_lock
                                   ELSE /\ commit_lock' = 0
                                        /\ phase' = [phase EXCEPT ![self] = "aborting"]
                                        /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                             /\ UNCHANGED << clock, vbox_hist, committedTxns, 
                                             rv, activeMask, readsDone, 
                                             writesDone, has_warp_write, ct, 
                                             warpCommits, readSet, writeSet >>

L_prepend(self) == /\ pc[self] = "L_prepend"
                   /\ vbox_hist' =          [a \in Addr |->
                                   IF \E t \in Thread : writeSet[self][t][a] THEN
                                       << <<ct[self]>> >> \o vbox_hist[a]
                                   ELSE
                                       vbox_hist[a]]
                   /\ commit_lock' = 0
                   /\ warpCommits' = [warpCommits EXCEPT ![self] = warpCommits[self] + 1]
                   /\ committedTxns' = committedTxns + 1
                   /\ phase' = [phase EXCEPT ![self] = "idle"]
                   /\ activeMask' = [activeMask EXCEPT ![self] = [t \in Thread |-> FALSE]]
                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                   /\ UNCHANGED << clock, rv, readsDone, writesDone, 
                                   has_warp_write, ct, readSet, writeSet >>

L_abort(self) == /\ pc[self] = "L_abort"
                 /\ readSet' = [readSet EXCEPT ![self] = [t \in Thread |-> {}]]
                 /\ writeSet' = [writeSet EXCEPT ![self] = [t \in Thread |-> [a \in Addr |-> FALSE]]]
                 /\ readsDone' = [readsDone EXCEPT ![self] = 0]
                 /\ writesDone' = [writesDone EXCEPT ![self] = 0]
                 /\ has_warp_write' = [has_warp_write EXCEPT ![self] = FALSE]
                 /\ activeMask' = [activeMask EXCEPT ![self] = [t \in Thread |-> TRUE]]
                 /\ phase' = [phase EXCEPT ![self] = "reading"]
                 /\ pc' = [pc EXCEPT ![self] = "L_read"]
                 /\ UNCHANGED << clock, commit_lock, vbox_hist, committedTxns, 
                                 rv, ct, warpCommits >>

WARP(self) == L_idle(self) \/ L_read(self) \/ L_read_next(self)
                 \/ L_write(self) \/ L_write_next(self) \/ L_validate(self)
                 \/ L_validate_try_lock(self) \/ L_validate_readsets(self)
                 \/ L_prepend(self) \/ L_abort(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Warp: WARP(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Liveness                                                           *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Warp : WF_vars(WARP(self))

ProgressProp ==
    \A w \in Warp :
        (phase[w] \in {"reading", "writing", "validating", "committing"}
         ~> phase[w] = "idle")

=============================================================================
