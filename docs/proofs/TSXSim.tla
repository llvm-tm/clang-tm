------------------------ MODULE TSXSim ------------------------
(*
 * TSX-Sim — Bloom-filter-based TSX Simulation Backend
 *
 * Algorithm (from runtime/tsx_sim/, AGENTS.md 2026-06-21):
 *
 *   TSX-Sim models Intel TSX execution at the cache-line granularity:
 *     1. Bloom-filter read-set.  Each thread has a 4096-bit bloom filter
 *        (double-hashing) recording all cache-line reads.  False positives
 *        are possible but no false negatives — safe for conflict detection.
 *     2. Cache-line write-set.  Tracks all written cache lines exactly
 *        (not a bloom filter), so no false positives for writes.
 *     3. Capacity abort.  If read-set exceeds MAX_READ_LINES or write-set
 *        exceeds MAX_WRITE_LINES, the TSX transaction aborts (simulating
 *        RTM capacity overflow, which occurs when L1 cache tracking
 *        capacity is exceeded).
 *     4. Conflict detection on commit.  For each cache line in the
 *        write-set, check every other thread's bloom filter.  If the
 *        bloom filter MAY-contain that cache line, signal a conflict
 *        abort (potential read-after-write hazard).  This is safe but
 *        may have false-positive unnecessary aborts.
 *     5. SGL fallback.  Same as TSXSGL: when retries exhausted or
 *        capacity exceeded, fall back to single global lock (SGL) mode.
 *     6. Virtual cycle counter.  Accumulates Skylake cycle costs per
 *        operation for simulation timing (modelled as per-event token
 *        counts for TLC model checking).
 *
 *   Conflict detection is CHECKED at commit time: the committing thread
 *   scans all other threads' bloom filters.  This is equivalent to
 *   eager conflict detection because:
 *     - A concurrent thread that read the line may abort due to the
 *       bloom-filter check at the first writer's commit.
 *     - If multiple threads attempt to commit concurrently, at most one
 *       succeeds (the first committer that finds no bloom match wins;
 *       the others abort).
 *
 * Invariants:
 *   TSXSafety:      No TSX runs while SGL is active (same as TSXSGL).
 *   LockExclusion:  At most one SGL at a time.
 *   NoFalseNeg:     If two transactions conflict (RW on same line),
 *                    at most one commits (bloom filter has no false neg.).
 *   CapacitySafe:   A TSX transaction never exceeds its capacity limits.
 *   ConflictSafe:   A TSX commit only succeeds if no concurrent reader.
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
    Thread,             (* Set of client thread IDs *)
    Addr,               (* Set of memory addresses *)
    CacheLine,          (* Set of cache-line identifiers *)
    HashPosition,       (* Set of bloom filter bit positions (0..BLOOM_SIZE-1) *)
    MAX_RETRIES,        (* Max TSX retries before SGL fallback *)
    MAX_READ_LINES,     (* Max cache lines in read-set before capacity abort *)
    MAX_WRITE_LINES     (* Max cache lines in write-set before capacity abort *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME CacheLine \subseteq Nat
ASSUME HashPosition \subseteq Nat
ASSUME MAX_RETRIES \in Nat
ASSUME MAX_READ_LINES \in Nat
ASSUME MAX_WRITE_LINES \in Nat

Hash == [cl \in {1, 2, 3, 4} |->
            IF cl = 1 THEN {1, 2}
            ELSE IF cl = 2 THEN {2, 3}
            ELSE IF cl = 3 THEN {4, 5}
            ELSE {5, 6}]
CacheLineOf == [a \in {1, 2, 3, 4, 5, 6, 7, 8} |->
                   IF a <= 2 THEN 1
                   ELSE IF a <= 4 THEN 2
                   ELSE IF a <= 6 THEN 3
                   ELSE 4]

VARIABLES
    (* ── Shared state ──────────────────────────────────────── *)
    mem,                (* [Addr -> Data]: shared memory *)
    sgl_lock,           (* 0 = free, t = locked by thread t *)

    (* ── Per-thread state ──────────────────────────────────── *)
    pc,                 (* [Thread -> {"idle", "tsx", "sgl"}] *)
    mode,               (* [Thread -> {"idle", "tsx", "sgl"}] *)

    (* ── TSX simulation state ──────────────────────────────── *)
    write_set,          (* [Thread -> Set(CacheLine)]: written cache lines *)
    write_data,         (* [Thread -> [CacheLine -> Data \cup {NoWrite}]] *)
    bloom,              (* [Thread -> Set(CacheLine)]: lines in bloom filter *)
    read_lines,         (* [Thread -> Set(CacheLine)]: all read lines (exact) *)

    (* ── SGL state ─────────────────────────────────────────── *)
    sgl_write_set,      (* [Thread -> Set(CacheLine)]: SGL-written cache lines *)
    sgl_write_data,     (* [Thread -> [CacheLine -> Data \cup {NoWrite}]] *)

    (* ── Bookkeeping ───────────────────────────────────────── *)
    tsx_retries,        (* [Thread -> Nat]: TSX retry count *)
    cycles,             (* [Thread -> Nat]: accumulated cycle cost *)
    committed,          (* [Thread -> Nat]: committed TX count *)
    aborted,            (* [Thread -> Nat]: aborted TX count *)
    capacity_aborts,    (* [Thread -> Nat]: capacity abort count *)
    conflict_aborts     (* [Thread -> Nat]: conflict abort count *)

vars == <<mem, sgl_lock, pc, mode, write_set, write_data, bloom,
         read_lines, sgl_write_set, sgl_write_data, tsx_retries,
         cycles, committed, aborted, capacity_aborts, conflict_aborts>>

NoWrite == 0 - 1
CL(a) == CacheLineOf[a]             (* shorthand: cache line of address *)

(*── Is the commit conflict-free relative to all other threads? ───*)
(*  Two checks: bloom filter for read conflicts, write-set overlap *)
(*  for write-write conflicts.  Consistent with real implementation *)
(*  which checks both bloom filters AND write-set overlap.         *)
ConflictFree(t) ==
    \A t2 \in Thread \ {t} :
        /\ \A cl \in write_set[t] :
             bloom[t2] \cap Hash[cl] = {}
        /\ write_set[t] \cap write_set[t2] = {}

(*── Capacity checks ────────────────────────────────────────────*)
ReadCapacityOK(t) ==
    Cardinality(read_lines[t]) <= MAX_READ_LINES

WriteCapacityOK(t) ==
    Cardinality(write_set[t]) <= MAX_WRITE_LINES

(*── Data write operations ──────────────────────────────────────*)
WriteMem(t, cl, v) ==
    (* Write v to all addresses in cache line cl (write-back) *)
    [a \in Addr |-> IF CL(a) = cl THEN v ELSE mem[a]]

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ mem = [a \in Addr |-> 0]
    /\ sgl_lock = 0
    /\ pc = [t \in Thread |-> "idle"]
    /\ mode = [t \in Thread |-> "idle"]
    /\ write_set = [t \in Thread |-> {}]
    /\ write_data = [t \in Thread |-> [cl \in CacheLine |-> NoWrite]]
    /\ bloom = [t \in Thread |-> {}]
    /\ read_lines = [t \in Thread |-> {}]
    /\ sgl_write_set = [t \in Thread |-> {}]
    /\ sgl_write_data = [t \in Thread |-> [cl \in CacheLine |-> NoWrite]]
    /\ tsx_retries = [t \in Thread |-> 0]
    /\ cycles = [t \in Thread |-> 0]
    /\ committed = [t \in Thread |-> 0]
    /\ aborted = [t \in Thread |-> 0]
    /\ capacity_aborts = [t \in Thread |-> 0]
    /\ conflict_aborts = [t \in Thread |-> 0]

(*--------------------------------------------------------------------*)
(* TSX Actions                                                        *)
(*--------------------------------------------------------------------*)

(*── TSX Begin: start hardware transaction ─────────────────────────*)
TSXBegin(t) ==
    /\ pc[t] = "idle"
    /\ tsx_retries[t] < MAX_RETRIES
    /\ sgl_lock = 0             (* SGL not active *)
    /\ pc' = [pc EXCEPT ![t] = "tsx"]
    /\ mode' = [mode EXCEPT ![t] = "tsx"]
    /\ write_set' = [write_set EXCEPT ![t] = {}]
    /\ write_data' = [write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ bloom' = [bloom EXCEPT ![t] = {}]
    /\ read_lines' = [read_lines EXCEPT ![t] = {}]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 20]   (* xbegin cost *)
    /\ tsx_retries' = [tsx_retries EXCEPT ![t] = 0]
    /\ UNCHANGED <<mem, sgl_lock, sgl_write_set, sgl_write_data,
                   committed, aborted, capacity_aborts, conflict_aborts>>

(*── TSX Read: load from memory, track in bloom + read_lines ──────*)
TSXRead(t, a) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ a \in Addr
    /\ Cardinality(read_lines[t] \cup {CL(a)}) <= MAX_READ_LINES
    /\ read_lines' = [read_lines EXCEPT ![t] = read_lines[t] \cup {CL(a)}]
    (* Update bloom filter: union of hash positions for this cache line *)
    /\ bloom' = [bloom EXCEPT ![t] = bloom[t] \cup Hash[CL(a)]]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 4]     (* L1 read cost *)
    /\ UNCHANGED <<mem, sgl_lock, pc, mode, write_set, write_data,
                   sgl_write_set, sgl_write_data, tsx_retries,
                   committed, aborted, capacity_aborts, conflict_aborts>>

(*── TSX Read capacity abort: read line limit exceeded ────────────*)
TSXReadCapacityAbort(t, a) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ a \in Addr
    /\ Cardinality(read_lines[t] \cup {CL(a)}) > MAX_READ_LINES
    (* Capacity exceeded — abort TSX *)
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ read_lines' = [read_lines EXCEPT ![t] = {}]
    /\ write_set' = [write_set EXCEPT ![t] = {}]
    /\ write_data' = [write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ bloom' = [bloom EXCEPT ![t] = {}]
    /\ tsx_retries' = [tsx_retries EXCEPT ![t] = tsx_retries[t] + 1]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 1500]  (* xabort cost *)
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ capacity_aborts' = [capacity_aborts EXCEPT ![t] = capacity_aborts[t] + 1]
    /\ UNCHANGED <<mem, sgl_lock, sgl_write_set, sgl_write_data,
                   committed, conflict_aborts>>

(*── TSX Write: buffer write in write_set ─────────────────────────*)
TSXWrite(t, a) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ a \in Addr
    /\ Cardinality(write_set[t] \cup {CL(a)}) <= MAX_WRITE_LINES
    /\ write_set' = [write_set EXCEPT ![t] = write_set[t] \cup {CL(a)}]
    /\ write_data' = [write_data EXCEPT ![t] =
                        [write_data[t] EXCEPT ![CL(a)] = mem[a]]]
    (* Bloom filter NOT updated for writes (reads only) *)
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 5]     (* L1 write cost *)
    /\ UNCHANGED <<mem, sgl_lock, pc, mode, bloom, read_lines,
                   sgl_write_set, sgl_write_data, tsx_retries,
                   committed, aborted, capacity_aborts, conflict_aborts>>

(*── TSX Write capacity abort: write line limit exceeded ──────────*)
TSXWriteCapacityAbort(t, a) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ a \in Addr
    /\ Cardinality(write_set[t] \cup {CL(a)}) > MAX_WRITE_LINES
    (* Capacity exceeded — abort TSX *)
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ read_lines' = [read_lines EXCEPT ![t] = {}]
    /\ write_set' = [write_set EXCEPT ![t] = {}]
    /\ write_data' = [write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ bloom' = [bloom EXCEPT ![t] = {}]
    /\ tsx_retries' = [tsx_retries EXCEPT ![t] = tsx_retries[t] + 1]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 1500]  (* xabort cost *)
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ capacity_aborts' = [capacity_aborts EXCEPT ![t] = capacity_aborts[t] + 1]
    /\ UNCHANGED <<mem, sgl_lock, sgl_write_set, sgl_write_data,
                   committed, conflict_aborts>>

(*── TSX Commit: check conflict-free, write-back, commit ──────────*)
TSXCommit(t) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ sgl_lock = 0                     (* SGL not active *)
    /\ ConflictFree(t)                  (* No conflict with other threads *)
    (* Write-back all write-set entries *)
    /\ LET write_all(m, ws, wd) ==
           [a \in Addr |->
               IF ws \cap {CL(a)} # {}
                THEN LET found == CHOOSE cl \in ws \cap {CL(a)} : TRUE IN
                         IF wd[found] # NoWrite THEN wd[found] ELSE m[a]
               ELSE m[a]]
       IN
       mem' = write_all(mem, write_set[t], write_data[t])
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ write_set' = [write_set EXCEPT ![t] = {}]
    /\ write_data' = [write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ bloom' = [bloom EXCEPT ![t] = {}]
    /\ read_lines' = [read_lines EXCEPT ![t] = {}]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 80]    (* xend cost *)
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ UNCHANGED <<sgl_lock, sgl_write_set, sgl_write_data, tsx_retries,
                   aborted, capacity_aborts, conflict_aborts>>

(*── TSX Conflict Abort: bloom/write-set detected conflict ──────────*)
TSXConflictAbort(t) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ ~ConflictFree(t)                 (* Conflict detected (bloom or write-set) *)
    (* Abort: discard write-set and read-set *)
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ write_set' = [write_set EXCEPT ![t] = {}]
    /\ write_data' = [write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ bloom' = [bloom EXCEPT ![t] = {}]
    /\ read_lines' = [read_lines EXCEPT ![t] = {}]
    /\ tsx_retries' = [tsx_retries EXCEPT ![t] = tsx_retries[t] + 1]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 2500]  (* conflict abort penalty *)
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ conflict_aborts' = [conflict_aborts EXCEPT ![t] = conflict_aborts[t] + 1]
    /\ UNCHANGED <<mem, sgl_lock, sgl_write_set, sgl_write_data,
                   committed, capacity_aborts>>

(*── TSX Retry: retry count increment (non-conflict, non-capacity abort) ──*)
TSXRetry(t) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ tsx_retries[t] < MAX_RETRIES
    /\ tsx_retries' = [tsx_retries EXCEPT ![t] = tsx_retries[t] + 1]
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 1500]  (* xabort (unknown reason) *)
    /\ UNCHANGED <<mem, sgl_lock, pc, mode, write_set, write_data,
                   bloom, read_lines, sgl_write_set, sgl_write_data,
                   committed, capacity_aborts, conflict_aborts>>

(*── TSX Fallback to SGL: retries exhausted ──────────────────────────*)
TSXFallback(t) ==
    /\ pc[t] = "tsx"
    /\ mode[t] = "tsx"
    /\ tsx_retries[t] >= MAX_RETRIES
    /\ sgl_lock = 0
    (* No other thread is in TSX mode — SGL writes would break TSX isolation *)
    /\ \A other \in Thread \ {t} : mode[other] # "tsx"
    /\ sgl_lock' = t
    /\ pc' = [pc EXCEPT ![t] = "sgl"]
    /\ mode' = [mode EXCEPT ![t] = "sgl"]
    (* Carry over write_set to SGL (re-acquire) *)
    /\ sgl_write_set' = [sgl_write_set EXCEPT ![t] = write_set[t]]
    /\ sgl_write_data' = [sgl_write_data EXCEPT ![t] = write_data[t]]
    /\ write_set' = [write_set EXCEPT ![t] = {}]
    /\ write_data' = [write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ bloom' = [bloom EXCEPT ![t] = {}]
    /\ read_lines' = [read_lines EXCEPT ![t] = {}]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 100]   (* SGL lock acquire *)
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ UNCHANGED <<mem, tsx_retries, committed,
                   capacity_aborts, conflict_aborts>>

(*--------------------------------------------------------------------*)
(* SGL Actions                                                        *)
(*--------------------------------------------------------------------*)

(*── SGL Begin: acquire global lock ──────────────────────────────────*)
SGLBegin(t) ==
    /\ pc[t] = "idle"
    /\ sgl_lock = 0
    (* No other thread is in TSX mode — hardware prevents parallel TSX+SGL *)
    /\ \A other \in Thread \ {t} : mode[other] # "tsx"
    /\ sgl_lock' = t
    /\ pc' = [pc EXCEPT ![t] = "sgl"]
    /\ mode' = [mode EXCEPT ![t] = "sgl"]
    /\ sgl_write_set' = [sgl_write_set EXCEPT ![t] = {}]
    /\ sgl_write_data' = [sgl_write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 100]   (* mutex lock *)
    /\ UNCHANGED <<mem, write_set, write_data, bloom, read_lines, tsx_retries,
                   committed, aborted, capacity_aborts, conflict_aborts>>

(*── SGL Read: direct load (lock provides isolation) ────────────────*)
SGLRead(t, a) ==
    /\ pc[t] = "sgl"
    /\ mode[t] = "sgl"
    /\ a \in Addr
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 4]     (* L1 read *)
    /\ UNCHANGED <<mem, sgl_lock, pc, mode, write_set, write_data,
                   bloom, read_lines, sgl_write_set, sgl_write_data,
                   tsx_retries, committed, aborted,
                   capacity_aborts, conflict_aborts>>

(*── SGL Write: direct store ─────────────────────────────────────────*)
SGLWrite(t, a) ==
    /\ pc[t] = "sgl"
    /\ mode[t] = "sgl"
    /\ a \in Addr
    /\ mem' = [mem EXCEPT ![a] = mem[a]]
    (* Also track in sgl_write_set for conflict-free commitment *)
    /\ sgl_write_set' = [sgl_write_set EXCEPT ![t] = sgl_write_set[t] \cup {CL(a)}]
    /\ sgl_write_data' = [sgl_write_data EXCEPT ![t] =
                            [sgl_write_data[t] EXCEPT ![CL(a)] = mem[a]]]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 6]     (* L1 write *)
    /\ UNCHANGED <<sgl_lock, pc, mode, write_set, write_data,
                   bloom, read_lines, tsx_retries, committed, aborted,
                   capacity_aborts, conflict_aborts>>

(*── SGL Commit: release lock ─────────────────────────────────────────*)
SGLCommit(t) ==
    /\ pc[t] = "sgl"
    /\ mode[t] = "sgl"
    /\ sgl_lock = t
    /\ sgl_lock' = 0
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ sgl_write_set' = [sgl_write_set EXCEPT ![t] = {}]
    /\ sgl_write_data' = [sgl_write_data EXCEPT ![t] = [cl \in CacheLine |-> NoWrite]]
    /\ cycles' = [cycles EXCEPT ![t] = cycles[t] + 75]    (* mutex unlock *)
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ UNCHANGED <<mem, write_set, write_data, bloom, read_lines, tsx_retries,
                   aborted, capacity_aborts, conflict_aborts>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \E t \in Thread :
        \/ TSXBegin(t)
        \/ (\E a \in Addr : TSXRead(t, a))
        \/ (\E a \in Addr : TSXReadCapacityAbort(t, a))
        \/ (\E a \in Addr : TSXWrite(t, a))
        \/ (\E a \in Addr : TSXWriteCapacityAbort(t, a))
        \/ TSXCommit(t)
        \/ TSXConflictAbort(t)
        \/ TSXRetry(t)
        \/ TSXFallback(t)
        \/ SGLBegin(t)
        \/ (\E a \in Addr : SGLRead(t, a))
        \/ (\E a \in Addr : SGLWrite(t, a))
        \/ SGLCommit(t)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: Lock-free exactly when no SGL is active (same as TSXSGL) ──*)
LockFreeInv ==
    (sgl_lock = 0) <=> ~(\E t \in Thread : mode[t] = "sgl")

(*── I2: Lock owner is the thread in SGL mode ─────────────────────────*)
LockOwnerInv ==
    \A t \in Thread : (sgl_lock = t) => (mode[t] = "sgl")

(*── I3: No TSX runs while SGL is active ────────────────────────────*)
TSXvsSGLSafety ==
    \A t \in Thread : (mode[t] = "tsx") => (sgl_lock = 0)

(*── I4: Bloom filter contains hash positions for all read lines ────*)
BloomContainsReads ==
    \A t \in Thread :
        \A cl \in read_lines[t] :
            Hash[cl] \subseteq bloom[t]

(*── I5: Capacity bounds are respected during TSX execution ────────*)
CapacityBounds ==
    \A t \in Thread :
        mode[t] = "tsx" =>
            /\ Cardinality(read_lines[t]) <= MAX_READ_LINES
            /\ Cardinality(write_set[t]) <= MAX_WRITE_LINES

(*── I6: Write-set is recorded in write_data and vice versa ─────────*)
WriteSetConsistent ==
    \A t \in Thread :
        /\ \A cl \in write_set[t] : write_data[t][cl] # NoWrite
        /\ \A cl \in CacheLine :
             write_data[t][cl] # NoWrite => cl \in write_set[t]

(*── I7: Conflict freedom — conflicting TSX can't both commit ─────*)
NoTSXCommitConflict ==
    \A t1, t2 \in Thread :
        (t1 # t2 /\
         \E cl \in write_set[t1] :
             ((\E cl2 \in read_lines[t2] : cl = cl2) \/
              (\E cl2 \in write_set[t2] : cl = cl2)))
        => ~(ConflictFree(t1) /\ ConflictFree(t2))
    (*
     * Both threads are in TSX mode and their access patterns
     * conflict (RAW, WAW, or WAR on same cache line).  The
     * ConflictFree(t) predicate checks both bloom filter
     * (for read conflicts) and write-set overlap (for
     * write-write conflicts).  At most one can be ConflictFree
     * at a time, so at most one can commit via TSXCommit.
     *
     * NOTE: In the real implementation, the bloom filter may
     * have false positives but never false negatives.  The TLA+
     * model uses a small bloom (6 bits, 2 hash functions) to
     * verify that the conflict detection is sound: if a conflict
     * exists, at least one thread's ConflictFree check fails.
     *)

(*── I8: SGL write-set does not conflict with committed TSX data ──*)
NoSGLTSXOverlap ==
    \A t1, t2 \in Thread :
        t1 # t2 /\
        mode[t1] = "sgl" /\
        sgl_write_set[t1] \cap write_set[t2] # {}
        => mode[t2] # "tsx"

(*── At most one SGL at a time ────────────────────────────────*)
LockExclusion ==
    \A t1, t2 \in Thread : (sgl_lock = t1 /\ sgl_lock = t2) => (t1 = t2)

(*====================================================================*)
(* Liveness (temporal)                                                 *)
(*====================================================================*)

(* Weak fairness: system eventually makes progress *)
Spec_WF == Spec /\ WF_vars(Next)

(*── Every transaction eventually commits or aborts ────────────────*)
TransactionProgress ==
    \A t \in Thread :
        []( (pc[t] \in {"tsx", "sgl"})
            => <><< committed[t]' > committed[t] \/ aborted[t]' > aborted[t] >>_vars )

(*====================================================================*)
(* THEOREMS                                                            *)
(*====================================================================*)

THEOREM LockExclusionInvariant ==
    Spec => []LockExclusion
    (* Every SGL is properly serialized *)

THEOREM TSXSafetyGuaranteed ==
    Spec => []NoSGLTSXOverlap
    (* SGL writes never conflict with active TSX transactions *)

THEOREM BloomCoverage ==
    Spec => []BloomContainsReads
    (* Bloom filter correctly encodes all read cache lines; this ensures
       ConflictFree checks against bloom[t2] cover all actual reads. *)

THEOREM NoTSXCommitConflictInvariant ==
    Spec => []NoTSXCommitConflict
    (* Conflicting TSX transactions cannot both commit *)

====
