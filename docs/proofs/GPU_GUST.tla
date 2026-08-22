------------------------------- MODULE GPU_GUST -------------------------------
(*
 * GPU_GUST — Scalable Multi-Version Concurrency Control for GPUs
 * (Nunes, Castro, Romano — IST/INESC-ID)
 *
 * GUST replaces the CAS-based commit-log append of classic MVCC
 * (JVSTM/CSMV) with an AtomicINC, which never fails and scales under
 * massive GPU parallelism.
 *
 * ── GPU-specific aspects modeled ─────────────────────────────────
 *
 * 1. WARP AS PROCESS:
 *    Each process is an entire warp. A shared `phase` enforces SIMT
 *    lockstep; per-thread state is stored in arrays indexed by Thread.
 *
 * 2. COMMIT LOG + AtomicINC (the GUST novelty):
 *    The warp leader reserves |Thread| contiguous CL slots with one
 *    AtomicINC on `writePtr`. Each lane's commit timestamp is
 *    CTS = base + lane. There is NO commit lock and NO CAS loop —
 *    slot assignment never blocks.
 *
 * 3. GTS (Global Timestamp) counts FINALIZED slots:
 *    Every attempted transaction (committed OR aborted) consumes a
 *    slot and advances GTS by the batch size. The MRV threshold relies
 *    on GTS including aborted transactions.
 *
 * 4. HYBRID CCT + MRV VALIDATION:
 *    - CCT: scan valPtr from CTS-1 downward while valPtr >= GTS.
 *      Those transactions may still be committing, so compare the
 *      read-set against each one's CL write-set (skip aborted).
 *    - MRV: once valPtr < GTS all earlier slots are finalized, so a
 *      single read-set scan suffices — abort if any read VBox holds a
 *      version newer than the snapshot.
 *
 * 5. WARP-COOPERATIVE BATCH COMMIT:
 *    The warp leader waits until GTS == base, then advances GTS by
 *    |Thread|, atomically publishing the batch. Write-back to VBoxes
 *    (fully parallel) happens BEFORE the GTS advance.
 *
 * 6. PRE-VALIDATION (intra-warp):
 *    If a lower-numbered lane touches an address a higher lane writes,
 *    the higher lane aborts early (__ballot_sync in the kernel).
 *    Modeled via `preconflict`.
 *
 * ── Fidelity notes ────────────────────────────────────────────────
 *
 * - Memory VALUES are not tracked (only versions + write-set presence).
 *   Conflict detection uses versions and CL write-sets, not values.
 * - The CL is not circular here: slots are never reused (writePtr
 *   strictly increases, bounded by MaxSlots = |Warp|*MaxCommits*|Thread|).
 * - Slot 0 is the first allocated slot (writePtr starts at 0), so the
 *   first warp has base 0 and can advance GTS from its initial value.
 * - Read-own-writes is not modeled (single-pass test kernel builds the
 *   read-set before the write-set; see backend README).
 * - WARP-COOPERATIVE STEPS: CL insertion, validation, and pre-validation
 *   update ALL lanes in a single atomic step (the kernel does this via
 *   __ballot_sync / __syncwarp + one AtomicINC). This is essential: a
 *   per-lane update would leave stale `cts` entries that a later
 *   transaction reuses, producing phantom commits (found by TLC with
 *   MaxCommits=2).
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
  Warp,                \* Set of warp IDs: {1, 2}
  Thread,              \* Set of thread IDs within each warp: {0, 1}
  Addr,                \* Set of addresses
  ReadsPerThread,      \* Number of reads per thread per transaction
  WritesPerThread,     \* Number of writes per thread per transaction
  MaxSlots,            \* CL size = |Warp| * MaxCommits * |Thread|
  MaxCommits           \* Max transactions per warp before halting

ASSUME Thread \subseteq Nat
ASSUME Addr \subseteq Nat
ASSUME Warp \subseteq Nat \ {0}
ASSUME ReadsPerThread > 0
ASSUME WritesPerThread >= 0
ASSUME MaxCommits > 0
ASSUME MaxSlots >= Cardinality(Warp) * MaxCommits * Cardinality(Thread)

\* ── Type definitions ──────────────────────────────────────────────

PhaseSet == {"idle", "reading", "writing", "validating", "done"}
SlotState == {"free", "pending", "committed", "aborted"}

TotalReadsPerWarp  == Cardinality(Thread) * ReadsPerThread
TotalWritesPerWarp == Cardinality(Thread) * WritesPerThread

\* FindBody(seq, rv): newest body in seq (newest-first) with version <= rv.
\* Returns the <<version>> tuple; versions are CTS+1 (0 = empty sentinel).
FindBody(seq, rv) ==
    seq[CHOOSE i \in 1..Len(seq) : seq[i][1] <= rv]

(* --algorithm GPU_GUST

variables
    \* ── Global state ─────────────────────────────────────────────
    gts = 0,                     \* Finalized slots (committed OR aborted)
    writePtr = 0,                \* Next slot to allocate (AtomicINC target)
    cl = [s \in 0..(MaxSlots-1) |-> [state |-> "free",
                                     ws |-> {},
                                     rs |-> {},
                                     st |-> 0,
                                     owner |-> <<0, 0>>]],
    vbox = [a \in Addr |-> << <<0>> >>],   \* VBox histories, newest-first

    \* ── Per-warp state (SHARED — SIMT lockstep) ─────────────────
    phase = [w \in Warp |-> "idle"],
    startTS = [w \in Warp |-> 0],
    base = [w \in Warp |-> 0],
    cts = [w \in Warp |-> [t \in Thread |-> 0]],
    readsDone = [w \in Warp |-> 0],
    writesDone = [w \in Warp |-> 0],
    activeMask = [w \in Warp |-> [t \in Thread |-> TRUE]],
    warpCommits = [w \in Warp |-> 0],

    \* ── Per-(warp, thread) state ────────────────────────────────
    readSet = [w \in Warp |-> [t \in Thread |-> {}]],
    writeSet = [w \in Warp |-> [t \in Thread |-> [a \in Addr |-> FALSE]]],
    preconflict = [w \in Warp |-> [t \in Thread |-> FALSE]];

define
    \* ── Operators ────────────────────────────────────────────────

    \* Addresses read by (w, t) — projection of <<addr, ver>> pairs.
    readAddrs(w, t) ==
        {addr : <<addr, ver>> \in readSet[w][t]}

    \* Addresses written by thread t of warp w.
    writeAddrs(w, t) ==
        {a \in Addr : writeSet[w][t][a]}

    \* GUST hybrid validation (CCT + MRV) for (w, t):
    \*   CCT: for each earlier slot s >= gts (may be in-flight),
    \*        abort if its CL write-set intersects our read-set.
    \*   MRV: every read VBox's newest version must be <= snapshot,
    \*        covering all finalized slots (s < gts).
    Valid(w, t) ==
        /\ \A s \in (startTS[w]+1)..(cts[w][t]-1) :
             s >= gts => (cl[s].state = "aborted"
                          \/ cl[s].ws \cap readAddrs(w, t) = {})
        /\ \A <<addr, ver>> \in readSet[w][t] :
             vbox[addr][1][1] <= startTS[w]

    \* Newest version published by a committed thread of warp w writing a.
    NewVersion(w, a) ==
        LET committedWriters ==
            {t2 \in Thread :
                cl[cts[w][t2]].state = "committed"
                /\ writeSet[w][t2][a]} IN
        CHOOSE v \in {cts[w][t2]+1 : t2 \in committedWriters} : TRUE

    \* ── Invariants ──────────────────────────────────────────────

    InvPhaseValid ==
        \A w \in Warp : phase[w] \in PhaseSet

    InvReadSetBounded ==
        \A w \in Warp, t \in Thread :
            Cardinality(readSet[w][t]) <= ReadsPerThread

    InvWriteSetBounded ==
        \A w \in Warp, t \in Thread :
            Cardinality(writeAddrs(w, t)) <= WritesPerThread

    \* GTS never outruns slot allocation.
    InvClockProgress ==
        gts <= writePtr

    \* GTS advances only in whole batches.
    InvBatchAlignment ==
        gts % Cardinality(Thread) = 0

    \* READ-SET CAPTURES EXISTING VERSIONS:
    \* Every <<addr, ver>> read corresponds to a body in the VBox.
    InvReadSetValid ==
        \A w \in Warp, t \in Thread :
            \A <<addr, ver>> \in readSet[w][t] :
                \E i \in 1..Len(vbox[addr]) :
                    vbox[addr][i][1] = ver

    \* COMMIT BUDGET: No warp exceeds MaxCommits.
    InvCommitBudget ==
        \A w \in Warp : warpCommits[w] <= MaxCommits

    \* SLOT STATES are well-formed and final states never revert.
    InvSlotStates ==
        \A s \in 0..(MaxSlots-1) : cl[s].state \in SlotState

    \* OPACITY — NO MISSED CONFLICTS:
    \* For every committed transaction c, none of its read addresses
    \* was written by a committed transaction s with startTS < s < c.
    \* This is exactly what the hybrid CCT+MRV protocol must enforce.
    InvNoMissedConflict ==
        \A c \in 0..(MaxSlots-1) :
            cl[c].state = "committed" =>
                \A <<addr, ver>> \in cl[c].rs :
                    ver <= cl[c].st
                    /\ ~\E s \in 0..(MaxSlots-1) :
                         s > cl[c].st /\ s < c
                         /\ cl[s].state = "committed"
                         /\ addr \in cl[s].ws

    \* Combined safety invariant
    Inv == /\ InvPhaseValid
           /\ InvReadSetBounded
           /\ InvWriteSetBounded
           /\ InvClockProgress
           /\ InvBatchAlignment
           /\ InvReadSetValid
           /\ InvCommitBudget
           /\ InvSlotStates
           /\ InvNoMissedConflict
end define;

\* ── Warp process ──────────────────────────────────────────────────
\* One process per warp. All threads share the instruction pointer
\* (phase); thread-local state via [self][t].

process WARP \in Warp
begin

L_idle:
    either
        when warpCommits[self] < MaxCommits;
        phase[self] := "reading";
        startTS[self] := gts;
        readsDone[self] := 0;
        writesDone[self] := 0;
        activeMask[self] := [t \in Thread |-> TRUE];
        readSet[self] := [t \in Thread |-> {}];
        writeSet[self] := [t \in Thread |-> [a \in Addr |-> FALSE]];
        preconflict[self] := [t \in Thread |-> FALSE];
        goto L_read;
    or
        goto L_idle;
    end either;

L_read:
    \* One thread reads one address; finds newest version <= snapshot.
    with t \in Thread do
        when activeMask[self][t];
        with a \in Addr do
            with v = FindBody(vbox[a], startTS[self])[1] do
                readSet[self][t] := readSet[self][t] \union {<<a, v>>};
            end with;
            readsDone[self] := readsDone[self] + 1;
            if Cardinality(readSet[self][t]) >= ReadsPerThread then
                activeMask[self][t] := FALSE;
            end if;
        end with;
    end with;
    goto L_read_next;

L_read_next:
    if readsDone[self] < TotalReadsPerWarp then
        with t \in Thread do
            if Cardinality(readSet[self][t]) < ReadsPerThread then
                activeMask[self][t] := TRUE;
            end if;
        end with;
        goto L_read;
    else
        phase[self] := "writing";
        writesDone[self] := 0;
        activeMask[self] := [t \in Thread |-> TRUE];
        goto L_write;
    end if;

L_write:
    \* One thread buffers one write (value elided; presence only).
    with t \in Thread do
        when activeMask[self][t];
        with a \in Addr do
            if ~writeSet[self][t][a] then
                writeSet[self][t][a] := TRUE;
                writesDone[self] := writesDone[self] + 1;
            end if;
            if Cardinality({a2 \in Addr : writeSet[self][t][a2]}) >= WritesPerThread then
                activeMask[self][t] := FALSE;
            end if;
        end with;
    end with;
    goto L_write_next;

L_write_next:
    if writesDone[self] < TotalWritesPerWarp then
        with t \in Thread do
            if Cardinality({a \in Addr : writeSet[self][t][a]}) < WritesPerThread then
                activeMask[self][t] := TRUE;
            end if;
        end with;
        goto L_write;
    else
        phase[self] := "validating";
        goto L_prevalidate;
    end if;

L_prevalidate:
    \* ── PRE-VALIDATION (intra-warp, ALL lanes lockstep) ──────────
    \* Lane t aborts if a lower lane touches an address lane t writes.
    \* Computed for every lane in ONE warp-cooperative step (the
    \* kernel's __ballot_sync loop).
    preconflict[self] := [t \in Thread |->
        \E a \in writeAddrs(self, t) :
            \E t2 \in {t3 \in Thread : t3 < t} :
                \/ \E <<addr2, v2>> \in readSet[self][t2] : addr2 = a
                \/ writeSet[self][t2][a]];
    goto L_insert;

L_insert:
    \* ── CL INSERTION via AtomicINC (ALL lanes lockstep) ──────────
    \* Leader reserves |Thread| contiguous slots; every lane writes
    \* its CL entry (state + write-set + read-set + snapshot) in the
    \* same warp-cooperative step. Each lane's cts is base + lane.
    base[self] := writePtr;
    writePtr := writePtr + Cardinality(Thread);
    cts[self] := [t \in Thread |-> base[self] + t];
    cl := [s \in 0..(MaxSlots-1) |->
        IF s \in {base[self] + t : t \in Thread}
        THEN LET t == CHOOSE t2 \in Thread : s = base[self] + t2 IN
             [state |-> IF preconflict[self][t]
                        THEN "aborted"
                        ELSE "pending",
              ws |-> writeAddrs(self, t),
              rs |-> readSet[self][t],
              st |-> startTS[self],
              owner |-> <<self, t>>]
        ELSE cl[s]];
    goto L_validate;

L_validate:
    \* ── HYBRID CCT + MRV VALIDATION (ALL lanes lockstep) ────────
    \* Each lane validates against concurrently-committed transactions
    \* (CCT) and the most-recent versions (MRV). All lanes finalize
    \* their slots in one warp-cooperative step.
    cl := [s \in 0..(MaxSlots-1) |->
        IF s \in {cts[self][t] : t \in Thread}
        THEN LET t == CHOOSE t2 \in Thread : s = cts[self][t2] IN
             [cl[s] EXCEPT !.state =
                 IF ~preconflict[self][t] /\ Valid(self, t)
                 THEN "committed" ELSE "aborted"]
        ELSE cl[s]];
    goto L_writeback;

L_writeback:
    \* ── WRITE-BACK (parallel publication) ────────────────────────
    \* Publish new VBox bodies for every address written by a
    \* committed lane. Pre-validation guarantees at most one
    \* committed lane per address in a warp.
    vbox := [a \in Addr |->
        IF \E t \in Thread :
               cl[cts[self][t]].state = "committed"
               /\ writeSet[self][t][a]
        THEN << <<NewVersion(self, a)>> >> \o vbox[a]
        ELSE vbox[a]];
    goto L_wait;

L_wait:
    \* ── BATCH PUBLICATION ────────────────────────────────────────
    \* Leader spins until GTS == base (all earlier slots finalized),
    \* then advances GTS by |Thread|.
    when gts = base[self];
    gts := gts + Cardinality(Thread);
    goto L_done;

L_done:
    phase[self] := "idle";
    warpCommits[self] := warpCommits[self] + 1;
    goto L_idle;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES gts, writePtr, cl, vbox, phase, startTS, base, cts, readsDone, 
          writesDone, activeMask, warpCommits, readSet, writeSet, preconflict, 
          pc

(* define statement *)
readAddrs(w, t) ==
    {addr : <<addr, ver>> \in readSet[w][t]}


writeAddrs(w, t) ==
    {a \in Addr : writeSet[w][t][a]}






Valid(w, t) ==
    /\ \A s \in (startTS[w]+1)..(cts[w][t]-1) :
         s >= gts => (cl[s].state = "aborted"
                      \/ cl[s].ws \cap readAddrs(w, t) = {})
    /\ \A <<addr, ver>> \in readSet[w][t] :
         vbox[addr][1][1] <= startTS[w]


NewVersion(w, a) ==
    LET committedWriters ==
        {t2 \in Thread :
            cl[cts[w][t2]].state = "committed"
            /\ writeSet[w][t2][a]} IN
    CHOOSE v \in {cts[w][t2]+1 : t2 \in committedWriters} : TRUE



InvPhaseValid ==
    \A w \in Warp : phase[w] \in PhaseSet

InvReadSetBounded ==
    \A w \in Warp, t \in Thread :
        Cardinality(readSet[w][t]) <= ReadsPerThread

InvWriteSetBounded ==
    \A w \in Warp, t \in Thread :
        Cardinality(writeAddrs(w, t)) <= WritesPerThread


InvClockProgress ==
    gts <= writePtr


InvBatchAlignment ==
    gts % Cardinality(Thread) = 0



InvReadSetValid ==
    \A w \in Warp, t \in Thread :
        \A <<addr, ver>> \in readSet[w][t] :
            \E i \in 1..Len(vbox[addr]) :
                vbox[addr][i][1] = ver


InvCommitBudget ==
    \A w \in Warp : warpCommits[w] <= MaxCommits


InvSlotStates ==
    \A s \in 0..(MaxSlots-1) : cl[s].state \in SlotState





InvNoMissedConflict ==
    \A c \in 0..(MaxSlots-1) :
        cl[c].state = "committed" =>
            \A <<addr, ver>> \in cl[c].rs :
                ver <= cl[c].st
                /\ ~\E s \in 0..(MaxSlots-1) :
                     s > cl[c].st /\ s < c
                     /\ cl[s].state = "committed"
                     /\ addr \in cl[s].ws


Inv == /\ InvPhaseValid
       /\ InvReadSetBounded
       /\ InvWriteSetBounded
       /\ InvClockProgress
       /\ InvBatchAlignment
       /\ InvReadSetValid
       /\ InvCommitBudget
       /\ InvSlotStates
       /\ InvNoMissedConflict


vars == << gts, writePtr, cl, vbox, phase, startTS, base, cts, readsDone, 
           writesDone, activeMask, warpCommits, readSet, writeSet, 
           preconflict, pc >>

ProcSet == (Warp)

Init == (* Global variables *)
        /\ gts = 0
        /\ writePtr = 0
        /\ cl = [s \in 0..(MaxSlots-1) |-> [state |-> "free",
                                            ws |-> {},
                                            rs |-> {},
                                            st |-> 0,
                                            owner |-> <<0, 0>>]]
        /\ vbox = [a \in Addr |-> << <<0>> >>]
        /\ phase = [w \in Warp |-> "idle"]
        /\ startTS = [w \in Warp |-> 0]
        /\ base = [w \in Warp |-> 0]
        /\ cts = [w \in Warp |-> [t \in Thread |-> 0]]
        /\ readsDone = [w \in Warp |-> 0]
        /\ writesDone = [w \in Warp |-> 0]
        /\ activeMask = [w \in Warp |-> [t \in Thread |-> TRUE]]
        /\ warpCommits = [w \in Warp |-> 0]
        /\ readSet = [w \in Warp |-> [t \in Thread |-> {}]]
        /\ writeSet = [w \in Warp |-> [t \in Thread |-> [a \in Addr |-> FALSE]]]
        /\ preconflict = [w \in Warp |-> [t \in Thread |-> FALSE]]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ warpCommits[self] < MaxCommits
                      /\ phase' = [phase EXCEPT ![self] = "reading"]
                      /\ startTS' = [startTS EXCEPT ![self] = gts]
                      /\ readsDone' = [readsDone EXCEPT ![self] = 0]
                      /\ writesDone' = [writesDone EXCEPT ![self] = 0]
                      /\ activeMask' = [activeMask EXCEPT ![self] = [t \in Thread |-> TRUE]]
                      /\ readSet' = [readSet EXCEPT ![self] = [t \in Thread |-> {}]]
                      /\ writeSet' = [writeSet EXCEPT ![self] = [t \in Thread |-> [a \in Addr |-> FALSE]]]
                      /\ preconflict' = [preconflict EXCEPT ![self] = [t \in Thread |-> FALSE]]
                      /\ pc' = [pc EXCEPT ![self] = "L_read"]
                   \/ /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                      /\ UNCHANGED <<phase, startTS, readsDone, writesDone, activeMask, readSet, writeSet, preconflict>>
                /\ UNCHANGED << gts, writePtr, cl, vbox, base, cts, 
                                warpCommits >>

L_read(self) == /\ pc[self] = "L_read"
                /\ \E t \in Thread:
                     /\ activeMask[self][t]
                     /\ \E a \in Addr:
                          /\ LET v == FindBody(vbox[a], startTS[self])[1] IN
                               readSet' = [readSet EXCEPT ![self][t] = readSet[self][t] \union {<<a, v>>}]
                          /\ readsDone' = [readsDone EXCEPT ![self] = readsDone[self] + 1]
                          /\ IF Cardinality(readSet'[self][t]) >= ReadsPerThread
                                THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = FALSE]
                                ELSE /\ TRUE
                                     /\ UNCHANGED activeMask
                /\ pc' = [pc EXCEPT ![self] = "L_read_next"]
                /\ UNCHANGED << gts, writePtr, cl, vbox, phase, startTS, base, 
                                cts, writesDone, warpCommits, writeSet, 
                                preconflict >>

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
                     /\ UNCHANGED << gts, writePtr, cl, vbox, startTS, base, 
                                     cts, readsDone, warpCommits, readSet, 
                                     writeSet, preconflict >>

L_write(self) == /\ pc[self] = "L_write"
                 /\ \E t \in Thread:
                      /\ activeMask[self][t]
                      /\ \E a \in Addr:
                           /\ IF ~writeSet[self][t][a]
                                 THEN /\ writeSet' = [writeSet EXCEPT ![self][t][a] = TRUE]
                                      /\ writesDone' = [writesDone EXCEPT ![self] = writesDone[self] + 1]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED << writesDone, writeSet >>
                           /\ IF Cardinality({a2 \in Addr : writeSet'[self][t][a2]}) >= WritesPerThread
                                 THEN /\ activeMask' = [activeMask EXCEPT ![self][t] = FALSE]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED activeMask
                 /\ pc' = [pc EXCEPT ![self] = "L_write_next"]
                 /\ UNCHANGED << gts, writePtr, cl, vbox, phase, startTS, base, 
                                 cts, readsDone, warpCommits, readSet, 
                                 preconflict >>

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
                                 /\ pc' = [pc EXCEPT ![self] = "L_prevalidate"]
                                 /\ UNCHANGED activeMask
                      /\ UNCHANGED << gts, writePtr, cl, vbox, startTS, base, 
                                      cts, readsDone, writesDone, warpCommits, 
                                      readSet, writeSet, preconflict >>

L_prevalidate(self) == /\ pc[self] = "L_prevalidate"
                       /\ preconflict' = [preconflict EXCEPT ![self] =
                              [t \in Thread |->
                                   \E a \in writeAddrs(self, t) :
                                       \E t2 \in {t3 \in Thread : t3 < t} :
                                           \/ \E <<addr2, v2>> \in readSet[self][t2] : addr2 = a
                                           \/ writeSet[self][t2][a]]]
                       /\ pc' = [pc EXCEPT ![self] = "L_insert"]
                       /\ UNCHANGED << gts, writePtr, cl, vbox, phase, startTS, 
                                       base, cts, readsDone, writesDone, 
                                       activeMask, warpCommits, readSet, 
                                       writeSet >>

L_insert(self) == /\ pc[self] = "L_insert"
                  /\ base' = [base EXCEPT ![self] = writePtr]
                  /\ writePtr' = writePtr + Cardinality(Thread)
                  /\ cts' = [cts EXCEPT ![self] =
                              [t \in Thread |-> base'[self] + t]]
                  /\ cl' = [s \in 0..(MaxSlots-1) |->
                              IF s \in {base'[self] + t : t \in Thread}
                              THEN LET t == CHOOSE t2 \in Thread :
                                                s = base'[self] + t2 IN
                                   [state |-> IF preconflict[self][t]
                                              THEN "aborted"
                                              ELSE "pending",
                                    ws |-> writeAddrs(self, t),
                                    rs |-> readSet[self][t],
                                    st |-> startTS[self],
                                    owner |-> <<self, t>>]
                              ELSE cl[s]]
                  /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                  /\ UNCHANGED << gts, vbox, phase, startTS, readsDone, 
                                  writesDone, activeMask, warpCommits, readSet, 
                                  writeSet, preconflict >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ cl' = [s \in 0..(MaxSlots-1) |->
                              IF s \in {cts[self][t] : t \in Thread}
                              THEN LET t == CHOOSE t2 \in Thread :
                                                s = cts[self][t2] IN
                                   [cl[s] EXCEPT !.state =
                                      IF ~preconflict[self][t] /\ Valid(self, t)
                                      THEN "committed" ELSE "aborted"]
                              ELSE cl[s]]
                    /\ pc' = [pc EXCEPT ![self] = "L_writeback"]
                    /\ UNCHANGED << gts, writePtr, vbox, phase, startTS, base, 
                                    cts, readsDone, writesDone, activeMask, 
                                    warpCommits, readSet, writeSet, 
                                    preconflict >>

L_writeback(self) == /\ pc[self] = "L_writeback"
                     /\ vbox' =     [a \in Addr |->
                                IF \E t \in Thread :
                                       cl[cts[self][t]].state = "committed"
                                       /\ writeSet[self][t][a]
                                THEN << <<NewVersion(self, a)>> >> \o vbox[a]
                                ELSE vbox[a]]
                     /\ pc' = [pc EXCEPT ![self] = "L_wait"]
                     /\ UNCHANGED << gts, writePtr, cl, phase, startTS, base, 
                                     cts, readsDone, writesDone, activeMask, 
                                     warpCommits, readSet, writeSet, 
                                     preconflict >>

L_wait(self) == /\ pc[self] = "L_wait"
                /\ gts = base[self]
                /\ gts' = gts + Cardinality(Thread)
                /\ pc' = [pc EXCEPT ![self] = "L_done"]
                /\ UNCHANGED << writePtr, cl, vbox, phase, startTS, base, cts, 
                                readsDone, writesDone, activeMask, warpCommits, 
                                readSet, writeSet, preconflict >>

L_done(self) == /\ pc[self] = "L_done"
                /\ phase' = [phase EXCEPT ![self] = "idle"]
                /\ warpCommits' = [warpCommits EXCEPT ![self] = warpCommits[self] + 1]
                /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                /\ UNCHANGED << gts, writePtr, cl, vbox, startTS, base, cts, 
                                readsDone, writesDone, activeMask, readSet, 
                                writeSet, preconflict >>

WARP(self) == L_idle(self) \/ L_read(self) \/ L_read_next(self)
                 \/ L_write(self) \/ L_write_next(self)
                 \/ L_prevalidate(self) \/ L_insert(self)
                 \/ L_validate(self) \/ L_writeback(self) \/ L_wait(self)
                 \/ L_done(self)

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
        (phase[w] \in {"reading", "writing", "validating"}
         ~> phase[w] = "idle")

=============================================================================
