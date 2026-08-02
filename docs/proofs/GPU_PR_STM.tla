--------------------------- MODULE GPU_PRIORITY_STM ---------------------------
(*
 * GPU PR-STM — PlusCal Specification (TLC-checkable)
 *
 * Algorithm: PR-STM (Priority Rule STM for GPUs)
 *   Shen et al., 2015 — priority-based lock acquisition for GPU warps.
 *
 * ── GPU-specific aspects modeled ──────────────────────────────────
 *
 * 1. WARP AS PROCESS (not thread):
 *    Each process represents an entire warp (32 threads in hardware).
 *    The warp manages an array of thread-local state (readSet[w][t],
 *    writeSet[w][t], etc.). The `phase` variable is SHARED at warp
 *    granularity, enforcing SIMT lockstep — all threads in a warp
 *    execute the same instruction. No thread can be validating while
 *    another reads.
 *
 *    CONTRAST with all 18 existing CPU STM models (TL2, SGL, TinySTM,
 *    etc.): those have one process per thread, each with its own
 *    state machine. Here, one process manages N threads.
 *
 * 2. PRIORITY-BASED LOCK TABLE:
 *    Each lock entry stores <<priority, version, locked>>. A warp may
 *    steal a lock only if its priority exceeds the current holder's.
 *    Models NVIDIA warp scheduling where higher-priority warps preempt
 *    lower-priority ones. Priority is static (warp ID).
 *
 * 3. LARGE READ-SET VALIDATION (O(threads × reads)):
 *    The warp read-set is the union of all threads' reads. Validation
 *    must check EVERY entry. With 32 threads × N reads each, this is
 *    the dominant cost. The model demonstrates conflict probability
 *    increasing linearly with |Thread| × ReadsPerThread.
 *
 * 4. WARP-LEVEL ABORT:
 *    If ANY thread's read is stale or ANY write lock is held by higher
 *    priority, the ENTIRE warp aborts. "One aborts, all abort."
 *    All threads' state is discarded atomically.
 *
 * 5. DIVERGENCE (fine-grained masking):
 *    activeMask[w][t] tracks which threads are executing the current
 *    path. Threads that finish their reads/writes early are masked
 *    out. Both paths execute sequentially within the warp.
 *
 * 6. COALESCED ACCESS (documented for future refinement):
 *    In hardware, consecutive addresses (a, a+4, a+8, ...) coalesce
 *    into one wide transaction. A refined model could group addresses
 *    by cache-line alignment.
 *
 * ── Fidelity notes ────────────────────────────────────────────────
 *
 * - Lock acquisition is bulk-atomic (all addresses acquired in one
 *   step). C++ PR-STM would use per-address atomicCAS. TLC state
 *   space prevents per-address labels; bulk is a conservative
 *   approximation.
 *
 * - Values are simplified to {0, 1}. Actual PR-STM computes
 *   increments/decrements; the model tracks only write presence.
 *
 * - Thread-local read/write counts are tracked per round, not per
 *   address. Each read step picks an address non-deterministically.
 *
 * - The model does not track the memory content of addresses
 *   (mem[a]) — only lock-table versions. This is sufficient for
 *   conflict detection (OCC does not check values, only versions).
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
  Warp,                \* Set of warp IDs: {0, 1}
  Thread,              \* Set of thread IDs within each warp: {0, 1}
  Addr,                \* Set of addresses: {a1, a2, a3, a4}
  LockTableSize,       \* Number of lock table entries (= Cardinality(Addr))
  ReadsPerThread,      \* Number of reads per thread per transaction
  WritesPerThread,     \* Number of writes per thread per transaction
  MaxCommits           \* Max committed transactions before halting

ASSUME Thread \subseteq Nat
ASSUME Addr \subseteq Nat
ASSUME Warp \subseteq Nat
ASSUME LockTableSize > 0
ASSUME ReadsPerThread > 0
ASSUME WritesPerThread >= 0
ASSUME MaxCommits > 0

\* ── GPU-specific type definitions ──────────────────────────────────

\* Lock table entry: <<priority, version, locked>>
\*   priority: static warp priority (0 = free, 1.. = warp ID + 1)
\*   version:  global clock value at last release
\*   locked:   TRUE if held by a warp
LCK_FREE  == <<0, 0, FALSE>>

\* Warp phases (shared by all threads in a warp — SIMT lockstep)
PhaseSet  == {"idle", "reading", "writing", "validating",
               "locking", "committing", "aborting"}

\* Hash: address → lock table index (simplified: direct index)
Hash(a) == a

\* Static priority per warp (w+1 so 0 = "free")
Priority(w) == w + 1

\* Total reads/writes per warp before phase transition
TotalReadsPerWarp  == Cardinality(Thread) * ReadsPerThread
TotalWritesPerWarp == Cardinality(Thread) * WritesPerThread

(*
--algorithm GPU_PRIORITY_STM

variables
  \* ── Shared state ──────────────────────────────────────────────

  \* Lock table: one entry per index, shared across all warps
  lockTable = [i \in 0..LockTableSize-1 |-> LCK_FREE],

  \* Global monotonic clock (incremented on each commit)
  globalClock = 0,

  \* Total committed transactions
  committedTxns = 0,

  \* ── Per-warp state (SHARED by all threads in the warp — LOCKSTEP) ──
  \* These variables are indexed by warp ID. The warp process reads
  \* and writes only its own slot. This enforces: "All threads in a
  \* warp are in the same phase at all times."

  \* Current warp phase (shared across all threads in this warp)
  phase = [w \in Warp |-> "idle"],

  \* Divergence mask: threads actively executing current path
  activeMask = [w \in Warp |-> [t \in Thread |-> FALSE]],

  \* Snapshot of globalClock at transaction start (for validation)
  startClock = [w \in Warp |-> 0],

  \* Read/write round counters (warp-level progress through phases)
  readsDone  = [w \in Warp |-> 0],
  writesDone = [w \in Warp |-> 0],

  \* ── Per-thread state (INDEXED by warp, then thread) ────────────

  \* Read-set: set of addresses read by thread t in warp w
  readSet = [w \in Warp |-> [t \in Thread |-> {}]],

  \* Version observed for each address at read time
  readVer = [w \in Warp |-> [t \in Thread |-> [a \in Addr |-> 0]]],

  \* Write-set: set of addresses written by thread t in warp w
  writeSet = [w \in Warp |-> [t \in Thread |-> {}]],

  \* Per-warp committed tx counter (bounded by MaxCommits)
  warpCommits = [w \in Warp |-> 0];

define
  \* ── GPU-specific invariants ───────────────────────────────────

  \* WARP PHASE COHERENCE:
  \* All threads in a warp share the same phase. This is structurally
  \* enforced (single phase[w] variable) but made explicit for TLC.
  InvWarpPhaseCoherent ==
      \A w \in Warp : phase[w] \in PhaseSet

  \* NO CROSS-PHASE WARPS (documentation invariant):
  \* A warp cannot have threads in different phases. This is the key
  \* structural difference from all 18 CPU STM models, where each
  \* thread has its own independent state machine.
  InvThreadsSharePhase ==
      \A w \in Warp :
          phase[w] \in {"reading", "writing"} =>
              \A t1, t2 \in Thread : TRUE
              \* Trivially true — single phase variable.
              \* Documenting the design decision.

  \* READ-SET SIZE BOUND:
  \* Total entries per thread ≤ ReadsPerThread + WritesPerThread.
  \* Models register pressure: GPU threads have finite registers.
  InvReadSetBounded ==
      \A w \in Warp, t \in Thread :
          Cardinality(readSet[w][t]) <= ReadsPerThread

  \* WRITE-SET SIZE BOUND (analogous)
  InvWriteSetBounded ==
      \A w \in Warp, t \in Thread :
          Cardinality(writeSet[w][t]) <= WritesPerThread

  \* LOCK COHERENCE: A locked entry has nonzero priority.
  InvLockConsistent ==
      \A i \in 0..LockTableSize-1 :
          lockTable[i][3] => (lockTable[i][1] > 0)
          \* locked => priority > 0

  \* FREE LOCK, FREE PRIORITY: Unlocked entries have priority 0.
  InvFreeLockNoOwner ==
      \A i \in 0..LockTableSize-1 :
          ~lockTable[i][3] => (lockTable[i][1] = 0)

  \* GLOBAL CLOCK MONOTONICITY
  InvClockMonotonic ==
      globalClock >= 0

  \* COMMIT BUDGET: No warp exceeds MaxCommits committed transactions.
  InvCommitBudget ==
      \A w \in Warp : warpCommits[w] <= MaxCommits

  \* ALL WARPS EVENTUALLY PROGRESS (liveness — requires --fair algorithm)
  ProgressProperty ==
      \A w \in Warp :
          (committedTxns > 0) ~> (committedTxns > 1)
end define;

\* ── Warp process ──────────────────────────────────────────────────
\*
\* KEY GPU DESIGN DECISION: One process per WARP, not per thread.
\* This models SIMT execution where all threads in a warp share an
\* instruction counter. The `phase[self]` variable is the warp's
\* instruction pointer — all threads are at this phase simultaneously.
\*
\* Thread-local state (readSet[self][t], writeSet[self][t], etc.) is
\* indexed by (warp, thread). The warp process manages this array.

process WARP \in Warp
    variable
        priority = Priority(self);  \* static priority: warp ID + 1

begin

L_idle:
    \* Non-deterministically begin a transaction or stay idle.
    \* Guard: only begin if commit budget remains (MaxCommits bound).
    either
        when warpCommits[self] < MaxCommits;
        \* ── Begin: Initialize all thread state for this warp ──
        phase[self] := "reading";
        readsDone[self] := 0;
        writesDone[self] := 0;
        startClock[self] := globalClock;
        with t \in Thread do
            readSet[self][t] := {};
            writeSet[self][t] := {};
            readVer[self][t] := [a \in Addr |-> 0];
        end with;
        goto L_read;
    or
        \* ── Stay idle ──
        goto L_idle;
    end either;

L_read:
    \* ── SIMT read step ──────────────────────────────────────────
    \*
    \* One atomic step: one thread in this warp reads one address.
    \* The `activeMask` tracks threads that still need to read.
    \* When all threads have completed ReadsPerThread reads, the
    \* warp transitions to the write phase.
    \*
    \* GPU NOTE: In real SIMT, all 32 threads execute their load
    \* instruction simultaneously. Modeling one thread per step
    \* gives TLC more interleavings (conservative approximation).
    \*
    \* LARGE READ-SET NOTE: With Thread=2 and ReadsPerThread=2,
    \* the total read-set per warp is 4 entries. Validation
    \* (L_validate) checks all 4. In hardware, 32 threads × N reads
    \* creates read-set pressure on register file and validation
    \* bandwidth.

    with t \in Thread do
        when activeMask[self][t];
        with a \in Addr do
            \* Record version at read time
            readSet[self][t] := readSet[self][t] \union {a};
            readVer[self][t][a] := lockTable[Hash(a)][2];
            readsDone[self] := readsDone[self] + 1;
            \* If this thread has read enough, mask it out
            if Cardinality(readSet[self][t]) >= ReadsPerThread then
                activeMask[self][t] := FALSE;
            end if;
        end with;
    end with;

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
        phase[self] := "writing";
        writesDone[self] := 0;
        activeMask[self] := [t \in Thread |-> TRUE];
        goto L_write;
    end if;

L_write:
    \* ── SIMT write step ─────────────────────────────────────────
    \*
    \* Analogous to L_read: one thread writes one address.
    \* The written address is recorded in writeSet for lock
    \* acquisition in L_lock.

    with t \in Thread do
        when activeMask[self][t];
        with a \in Addr do
            writeSet[self][t] := writeSet[self][t] \union {a};
            writesDone[self] := writesDone[self] + 1;
            if Cardinality(writeSet[self][t]) >= WritesPerThread then
                activeMask[self][t] := FALSE;
            end if;
        end with;
    end with;

L_write_next:
    if writesDone[self] < TotalWritesPerWarp then
        with t \in Thread do
            if Cardinality(writeSet[self][t]) < WritesPerThread then
                activeMask[self][t] := TRUE;
            end if;
        end with;
        goto L_write;
    else
        \* All threads done writing → move to validation
        phase[self] := "validating";
        goto L_validate;
    end if;

L_validate:
    \* ── WARP-LEVEL VALIDATION ────────────────────────────────────
    \*
    \* If ANY entry has changed or is locked, the ENTIRE warp aborts.

    if \E t \in Thread : \E a \in readSet[self][t] :
            lockTable[Hash(a)][2] # readVer[self][t][a] \/
            lockTable[Hash(a)][3] then
        goto L_abort;
    else
        \* Validation passed
        goto L_validate_pass;
    end if;

L_validate_pass:
    phase[self] := "locking";
    goto L_lock_check;

L_lock_check:
    \* ── PRIORITY CHECK ──────────────────────────────────────────
    \* If any needed lock is held by higher priority, abort.
    if \E t \in Thread : \E a \in writeSet[self][t] :
            lockTable[Hash(a)][3] /\
            lockTable[Hash(a)][1] > priority then
        goto L_abort;
    else
        goto L_lock_acquire;
    end if;

L_lock_acquire:
    \* Acquire all needed locks (atomic functional update)
    lockTable := [i \in 0..LockTableSize-1 |->
        IF \E t \in Thread : i \in writeSet[self][t]
        THEN <<priority, lockTable[i][2], TRUE>>
        ELSE lockTable[i]
    ];
    goto L_commit;

L_commit:
    \* ── COMMIT: Clock increment, write-back ─────────────────────
    phase[self] := "committing";
    globalClock := globalClock + 1;

    lockTable := [i \in 0..LockTableSize-1 |->
        IF \E t \in Thread : i \in writeSet[self][t]
        THEN <<0, globalClock, FALSE>>
        ELSE lockTable[i]
    ];
    goto L_commit_finish;

L_commit_finish:
    warpCommits[self] := warpCommits[self] + 1;
    committedTxns := committedTxns + 1;
    phase[self] := "idle";
    activeMask[self] := [t \in Thread |-> FALSE];
    goto L_idle;

L_abort:
    \* ── WARP-LEVEL ABORT ─────────────────────────────────────────
    \*
    \* GPU-SPECIFIC: ALL threads' state is discarded atomically.
    \* The entire warp retries from the read phase.
    \*
    \* CONTRAST with CPU: a CPU thread can abort independently
    \* while sibling threads continue. On GPU, the warp is the
    \* smallest schedulable unit — all threads abort together.

    with t \in Thread do
        readSet[self][t] := {};
        writeSet[self][t] := {};
        readVer[self][t] := [a \in Addr |-> 0];
    end with;
    readsDone[self] := 0;
    writesDone[self] := 0;
    activeMask[self] := [t \in Thread |-> TRUE];
    phase[self] := "reading";
    goto L_read;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES lockTable, globalClock, committedTxns, phase, activeMask, 
          startClock, readsDone, writesDone, readSet, readVer, writeSet, 
          warpCommits, pc

(* define statement *)
InvWarpPhaseCoherent ==
    \A w \in Warp : phase[w] \in PhaseSet





InvThreadsSharePhase ==
    \A w \in Warp :
        phase[w] \in {"reading", "writing"} =>
            \A t1, t2 \in Thread : TRUE






InvReadSetBounded ==
    \A w \in Warp, t \in Thread :
        Cardinality(readSet[w][t]) <= ReadsPerThread


InvWriteSetBounded ==
    \A w \in Warp, t \in Thread :
        Cardinality(writeSet[w][t]) <= WritesPerThread


InvLockConsistent ==
    \A i \in 0..LockTableSize-1 :
        lockTable[i][3] => (lockTable[i][1] > 0)



InvFreeLockNoOwner ==
    \A i \in 0..LockTableSize-1 :
        ~lockTable[i][3] => (lockTable[i][1] = 0)


InvClockMonotonic ==
    globalClock >= 0


InvCommitBudget ==
    \A w \in Warp : warpCommits[w] <= MaxCommits


ProgressProperty ==
    \A w \in Warp :
        (committedTxns > 0) ~> (committedTxns > 1)

VARIABLE priority

vars == << lockTable, globalClock, committedTxns, phase, activeMask, 
           startClock, readsDone, writesDone, readSet, readVer, writeSet, 
           warpCommits, pc, priority >>

ProcSet == (Warp)

Init == (* Global variables *)
        /\ lockTable = [i \in 0..LockTableSize-1 |-> LCK_FREE]
        /\ globalClock = 0
        /\ committedTxns = 0
        /\ phase = [w \in Warp |-> "idle"]
        /\ activeMask = [w \in Warp |-> [t \in Thread |-> FALSE]]
        /\ startClock = [w \in Warp |-> 0]
        /\ readsDone = [w \in Warp |-> 0]
        /\ writesDone = [w \in Warp |-> 0]
        /\ readSet = [w \in Warp |-> [t \in Thread |-> {}]]
        /\ readVer = [w \in Warp |-> [t \in Thread |-> [a \in Addr |-> 0]]]
        /\ writeSet = [w \in Warp |-> [t \in Thread |-> {}]]
        /\ warpCommits = [w \in Warp |-> 0]
        (* Process WARP *)
        /\ priority = [self \in Warp |-> Priority(self)]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ warpCommits[self] < MaxCommits
                      /\ phase' = [phase EXCEPT ![self] = "reading"]
                      /\ readsDone' = [readsDone EXCEPT ![self] = 0]
                      /\ writesDone' = [writesDone EXCEPT ![self] = 0]
                      /\ startClock' = [startClock EXCEPT ![self] = globalClock]
                      /\ \E t \in Thread:
                           /\ readSet' = [readSet EXCEPT ![self][t] = {}]
                           /\ writeSet' = [writeSet EXCEPT ![self][t] = {}]
                           /\ readVer' = [readVer EXCEPT ![self][t] = [a \in Addr |-> 0]]
                      /\ pc' = [pc EXCEPT ![self] = "L_read"]
                   \/ /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                      /\ UNCHANGED <<phase, startClock, readsDone, writesDone, readSet, readVer, writeSet>>
                /\ UNCHANGED << lockTable, globalClock, committedTxns, 
                                activeMask, warpCommits, priority >>

L_read(self) == /\ pc[self] = "L_read"
                /\ \E t \in Thread:
                     /\ activeMask[self][t]
                     /\ \E a \in Addr:
                          /\ readSet' = [readSet EXCEPT ![self][t] = readSet[self][t] \union {a}]
                          /\ readVer' = [readVer EXCEPT ![self][t][a] = lockTable[Hash(a)][2]]
                          /\ readsDone' = [readsDone EXCEPT ![self] = readsDone[self] + 1]
                          /\ IF Cardinality(readSet'[self][t]) >= ReadsPerThread
                                THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = FALSE]
                                ELSE /\ TRUE
                                     /\ UNCHANGED activeMask
                /\ pc' = [pc EXCEPT ![self] = "L_read_next"]
                /\ UNCHANGED << lockTable, globalClock, committedTxns, phase, 
                                startClock, writesDone, writeSet, warpCommits, 
                                priority >>

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
                     /\ UNCHANGED << lockTable, globalClock, committedTxns, 
                                     startClock, readsDone, readSet, readVer, 
                                     writeSet, warpCommits, priority >>

L_write(self) == /\ pc[self] = "L_write"
                 /\ \E t \in Thread:
                      /\ activeMask[self][t]
                      /\ \E a \in Addr:
                           /\ writeSet' = [writeSet EXCEPT ![self][t] = writeSet[self][t] \union {a}]
                           /\ writesDone' = [writesDone EXCEPT ![self] = writesDone[self] + 1]
                           /\ IF Cardinality(writeSet'[self][t]) >= WritesPerThread
                                 THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = FALSE]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED activeMask
                 /\ pc' = [pc EXCEPT ![self] = "L_write_next"]
                 /\ UNCHANGED << lockTable, globalClock, committedTxns, phase, 
                                 startClock, readsDone, readSet, readVer, 
                                 warpCommits, priority >>

L_write_next(self) == /\ pc[self] = "L_write_next"
                      /\ IF writesDone[self] < TotalWritesPerWarp
                            THEN /\ \E t \in Thread:
                                      IF Cardinality(writeSet[self][t]) < WritesPerThread
                                         THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = TRUE]
                                         ELSE /\ TRUE
                                              /\ UNCHANGED activeMask
                                 /\ pc' = [pc EXCEPT ![self] = "L_write"]
                                 /\ phase' = phase
                            ELSE /\ phase' = [phase EXCEPT ![self] = "validating"]
                                 /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                                 /\ UNCHANGED activeMask
                      /\ UNCHANGED << lockTable, globalClock, committedTxns, 
                                      startClock, readsDone, writesDone, 
                                      readSet, readVer, writeSet, warpCommits, 
                                      priority >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF \E t \in Thread : \E a \in readSet[self][t] :
                               lockTable[Hash(a)][2] # readVer[self][t][a] \/
                               lockTable[Hash(a)][3]
                          THEN /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "L_validate_pass"]
                    /\ UNCHANGED << lockTable, globalClock, committedTxns, 
                                    phase, activeMask, startClock, readsDone, 
                                    writesDone, readSet, readVer, writeSet, 
                                    warpCommits, priority >>

L_validate_pass(self) == /\ pc[self] = "L_validate_pass"
                         /\ phase' = [phase EXCEPT ![self] = "locking"]
                         /\ pc' = [pc EXCEPT ![self] = "L_lock_check"]
                         /\ UNCHANGED << lockTable, globalClock, committedTxns, 
                                         activeMask, startClock, readsDone, 
                                         writesDone, readSet, readVer, 
                                         writeSet, warpCommits, priority >>

L_lock_check(self) == /\ pc[self] = "L_lock_check"
                      /\ IF \E t \in Thread : \E a \in writeSet[self][t] :
                                 lockTable[Hash(a)][3] /\
                                 lockTable[Hash(a)][1] > priority[self]
                            THEN /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                            ELSE /\ pc' = [pc EXCEPT ![self] = "L_lock_acquire"]
                      /\ UNCHANGED << lockTable, globalClock, committedTxns, 
                                      phase, activeMask, startClock, readsDone, 
                                      writesDone, readSet, readVer, writeSet, 
                                      warpCommits, priority >>

L_lock_acquire(self) == /\ pc[self] = "L_lock_acquire"
                        /\ lockTable' =              [i \in 0..LockTableSize-1 |->
                                            IF \E t \in Thread : i \in writeSet[self][t]
                                            THEN <<priority[self], lockTable[i][2], TRUE>>
                                            ELSE lockTable[i]
                                        ]
                        /\ pc' = [pc EXCEPT ![self] = "L_commit"]
                        /\ UNCHANGED << globalClock, committedTxns, phase, 
                                        activeMask, startClock, readsDone, 
                                        writesDone, readSet, readVer, writeSet, 
                                        warpCommits, priority >>

L_commit(self) == /\ pc[self] = "L_commit"
                  /\ phase' = [phase EXCEPT ![self] = "committing"]
                  /\ globalClock' = globalClock + 1
                  /\ lockTable' =              [i \in 0..LockTableSize-1 |->
                                      IF \E t \in Thread : i \in writeSet[self][t]
                                      THEN <<0, globalClock', FALSE>>
                                      ELSE lockTable[i]
                                  ]
                  /\ pc' = [pc EXCEPT ![self] = "L_commit_finish"]
                  /\ UNCHANGED << committedTxns, activeMask, startClock, 
                                  readsDone, writesDone, readSet, readVer, 
                                  writeSet, warpCommits, priority >>

L_commit_finish(self) == /\ pc[self] = "L_commit_finish"
                         /\ warpCommits' = [warpCommits EXCEPT ![self] = warpCommits[self] + 1]
                         /\ committedTxns' = committedTxns + 1
                         /\ phase' = [phase EXCEPT ![self] = "idle"]
                         /\ activeMask' = [activeMask EXCEPT ![self] = [t \in Thread |-> FALSE]]
                         /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                         /\ UNCHANGED << lockTable, globalClock, startClock, 
                                         readsDone, writesDone, readSet, 
                                         readVer, writeSet, priority >>

L_abort(self) == /\ pc[self] = "L_abort"
                 /\ \E t \in Thread:
                      /\ readSet' = [readSet EXCEPT ![self][t] = {}]
                      /\ writeSet' = [writeSet EXCEPT ![self][t] = {}]
                      /\ readVer' = [readVer EXCEPT ![self][t] = [a \in Addr |-> 0]]
                 /\ readsDone' = [readsDone EXCEPT ![self] = 0]
                 /\ writesDone' = [writesDone EXCEPT ![self] = 0]
                 /\ activeMask' = [activeMask EXCEPT ![self] = [t \in Thread |-> TRUE]]
                 /\ phase' = [phase EXCEPT ![self] = "reading"]
                 /\ pc' = [pc EXCEPT ![self] = "L_read"]
                 /\ UNCHANGED << lockTable, globalClock, committedTxns, 
                                 startClock, warpCommits, priority >>

WARP(self) == L_idle(self) \/ L_read(self) \/ L_read_next(self)
                 \/ L_write(self) \/ L_write_next(self) \/ L_validate(self)
                 \/ L_validate_pass(self) \/ L_lock_check(self)
                 \/ L_lock_acquire(self) \/ L_commit(self)
                 \/ L_commit_finish(self) \/ L_abort(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Warp: WARP(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* ── Model configuration notes ──────────────────────────────────────
\*
\* Minimal config (for quick TLC check):
\*   Warp           = {0, 1}
\*   Thread         = {0, 1}       -- 2 threads per warp, 4 total
\*   Addr           = {0, 1, 2, 3}
\*   LockTableSize  = 4
\*   ReadsPerThread = 2            -- 4 entries per warp read-set
\*   WritesPerThread = 1           -- 2 entries per warp write-set
\*   MaxCommits     = 2
\*
\* With these settings, total distinct states ~ 50K–150K depending
\* on interleavings. TLC completes in ~5–15 seconds.
\*
\* Large config (demonstrates read-set scaling):
\*   Thread         = {0, 1, 2, 3} -- 4 threads per warp, 8 total
\*   ReadsPerThread = 4            -- 16 entries per warp read-set
\*   State space grows exponentially — may need -Xmx4g.
\*
\* =====================================================================

====
