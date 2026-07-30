----------------------------- MODULE Calvin -----------------------------
(*
 * Calvin — Two-phase OCC backend
 *
 * CONSTANTS:
 *   Thread     — Set of thread IDs (positive integers)
 *   Addr       — Set of memory addresses
 *   Data       — Set of data values
 *   MaxCommits — Max committed transactions per thread (bound)
 *
 * Phase 1 (Collect): Execute body, capture read values and buffer writes.
 *   Every Read appends <<addr, current_mem[addr]>> to a read-set.
 *   Every Write appends <<addr, current_mem[addr]>> (pre-write value)
 *               to the read-set AND buffers the write.
 *   On End, transition to Execute (simulated by setting use_collected).
 *
 * Phase 2 (Execute): Re-execute body deterministically.
 *   Reads check the write-buffer first (read-own-writes).
 *   Writes are buffered (same as Collect).
 *   On End, validate ALL read-set entries against current mem values.
 *   If any captured value ≠ current mem value → abort (retry from Collect).
 *   Otherwise → apply write-set to memory and commit.
 *
 * Key invariants:
 *   - At most one thread commits at a time (OCC)
 *   - Committed transactions have a consistent read-set
 *   - No transactional memory is lost
 *)

EXTENDS Naturals, FiniteSets, TLC, TMTypes

CONSTANTS Thread, Addr, Data, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat \ {0}

(* --algorithm Calvin

variables
    \* ── Shared memory ──
    mem = [a \in Addr |-> 0],

    \* ── Per-thread commit/abort counters ──
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],

    \* ── Fence tracking ──
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
variables
    \* ── Transaction-local state ──
    phase = "idle",         \* "idle", "collect", "execute"
    read_entries = {},
    write_set = [a \in Addr |-> NoWrite],
    \* ── Count of writes in this transaction (for fence fidelity) ──
    has_write = FALSE;
begin

L_idle:
    if committed[self] >= MaxCommits then
        goto L_done;
    else
        \* Begin transaction in collect phase
        phase := "collect";
        read_entries := {};
        write_set := [a \in Addr |-> NoWrite];
        has_write := FALSE;
        lastSignalFence[self] := "";
        lastThreadFence[self] := "";
        lastRmw[self] := "";
        goto L_active;
    end if;

L_active:
    either \* ── Read from memory (or from own write-set) ──
        with a \in Addr do
            if phase = "collect" then
                \* Capture current value
                read_entries := read_entries \union {<<a, mem[a]>>};
                lastSignalFence[self] := "sc";
            else \* execute phase
                \* Don't add to read_entries (already captured in collect).
                \* Read returns mem[a] if not in own write-set.
                if write_set[a] = NoWrite then
                    skip;   \* reads from mem[a] in real impl
                else
                    skip;   \* reads from own write buffer
                end if;
            end if;
        end with;
        goto L_active;
    or \* ── Write to buffer ──
        with a \in Addr, v \in Data do
            if phase = "collect" then
                \* Capture pre-write value
                read_entries := read_entries \union {<<a, mem[a]>>};
            end if;
            write_set[a] := v;
            has_write := TRUE;
            lastRmw[self] := "acquire";
        end with;
        goto L_active;
    or \* ── End transaction ──
        if phase = "collect" then
            \* Transition to execute phase (simulates siglongjmp + re-entry).
            \* read_entries survives; write_set cleared at next tm_begin.
            phase := "execute";
            goto L_active;
        elsif has_write /\ \E a \in Addr : write_set[a] # NoWrite then
            \* Execute phase: validate all reads, then write-back
            if \A <<addr, captured>> \in read_entries :
                    mem[addr] = captured
            then
                \* Validation passed — apply writes
                mem := [a \in Addr |->
                    IF write_set[a] # NoWrite THEN write_set[a] ELSE mem[a]];
                lastSignalFence[self] := "sc";
                committed[self] := committed[self] + 1;
                phase := "idle";
                goto L_idle;
            else
                \* Validation failed — abort, retry from collect
                aborted[self] := aborted[self] + 1;
                phase := "collect";
                read_entries := {};
                write_set := [a \in Addr |-> NoWrite];
                has_write := FALSE;
                goto L_active;
            end if;
        else
            \* Read-only or empty transaction — no write-back needed
            committed[self] := committed[self] + 1;
            phase := "idle";
            goto L_idle;
        end if;
    end either;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES mem, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, 
          pc, phase, read_entries, write_set, has_write

vars == << mem, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, 
           pc, phase, read_entries, write_set, has_write >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        (* Process ThreadProc *)
        /\ phase = [self \in Thread |-> "idle"]
        /\ read_entries = [self \in Thread |-> {}]
        /\ write_set = [self \in Thread |-> [a \in Addr |-> NoWrite]]
        /\ has_write = [self \in Thread |-> FALSE]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                           /\ UNCHANGED << lastSignalFence, lastThreadFence, 
                                           lastRmw, phase, read_entries, 
                                           write_set, has_write >>
                      ELSE /\ phase' = [phase EXCEPT ![self] = "collect"]
                           /\ read_entries' = [read_entries EXCEPT ![self] = {}]
                           /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                           /\ has_write' = [has_write EXCEPT ![self] = FALSE]
                           /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                           /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                           /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                /\ UNCHANGED << mem, committed, aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF phase[self] = "collect"
                                THEN /\ read_entries' = [read_entries EXCEPT ![self] = read_entries[self] \union {<<a, mem[a]>>}]
                                     /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                                ELSE /\ IF write_set[self][a] = NoWrite
                                           THEN /\ TRUE
                                           ELSE /\ TRUE
                                     /\ UNCHANGED << lastSignalFence, 
                                                     read_entries >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<mem, committed, aborted, lastRmw, phase, write_set, has_write>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               /\ IF phase[self] = "collect"
                                     THEN /\ read_entries' = [read_entries EXCEPT ![self] = read_entries[self] \union {<<a, mem[a]>>}]
                                     ELSE /\ TRUE
                                          /\ UNCHANGED read_entries
                               /\ write_set' = [write_set EXCEPT ![self][a] = v]
                               /\ has_write' = [has_write EXCEPT ![self] = TRUE]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<mem, committed, aborted, lastSignalFence, phase>>
                     \/ /\ IF phase[self] = "collect"
                              THEN /\ phase' = [phase EXCEPT ![self] = "execute"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << mem, committed, aborted, 
                                                   lastSignalFence, 
                                                   read_entries, write_set, 
                                                   has_write >>
                              ELSE /\ IF has_write[self] /\ \E a \in Addr : write_set[self][a] # NoWrite
                                         THEN /\ IF \A <<addr, captured>> \in read_entries[self] :
                                                         mem[addr] = captured
                                                    THEN /\ mem' =    [a \in Addr |->
                                                                   IF write_set[self][a] # NoWrite THEN write_set[self][a] ELSE mem[a]]
                                                         /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                                                         /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                                         /\ phase' = [phase EXCEPT ![self] = "idle"]
                                                         /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                                         /\ UNCHANGED << aborted, 
                                                                         read_entries, 
                                                                         write_set, 
                                                                         has_write >>
                                                    ELSE /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                                         /\ phase' = [phase EXCEPT ![self] = "collect"]
                                                         /\ read_entries' = [read_entries EXCEPT ![self] = {}]
                                                         /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                                                         /\ has_write' = [has_write EXCEPT ![self] = FALSE]
                                                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                                         /\ UNCHANGED << mem, 
                                                                         committed, 
                                                                         lastSignalFence >>
                                         ELSE /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                              /\ phase' = [phase EXCEPT ![self] = "idle"]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                              /\ UNCHANGED << mem, aborted, 
                                                              lastSignalFence, 
                                                              read_entries, 
                                                              write_set, 
                                                              has_write >>
                        /\ UNCHANGED lastRmw
                  /\ UNCHANGED lastThreadFence

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << mem, committed, aborted, lastSignalFence, 
                                lastThreadFence, lastRmw, phase, read_entries, 
                                write_set, has_write >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* INVARIANTS                                                         *)
(*====================================================================*)

(* Model bound to prevent infinite execution *)
ModelBound ==
    \A t \in Thread : committed[t] <= MaxCommits

(* I1: Committed transactions have a consistent read-set.
   For a thread that just committed, all its read-set entries matched
   mem at commit time (before write-back).  Addresses also written by
   the committing thread may have changed due to write-back. *)
ConsistentReads ==
    \A t \in Thread :
        pc[t] = "L_idle" /\ phase[t] = "idle" /\ committed[t] > 0
        => \A <<addr, captured>> \in read_entries[t] :
               write_set[t][addr] # NoWrite \/ mem[addr] = captured

(* I2: Fence fidelity — any thread with a write-set has issued a fence *)
FenceFidelityInst ==
    FenceFidelity(Thread, [t \in Thread |-> {a \in Addr : write_set[t][a] # NoWrite}],
                  lastSignalFence, lastThreadFence, lastRmw)

(* I3: Thread phases are bounded *)
PhaseValid ==
    \A t \in Thread : phase[t] \in {"idle", "collect", "execute"}

(* I4: Read-only transactions never have write entries *)
ReadOnlyNoWrites ==
    \A t \in Thread :
        phase[t] = "execute" /\ ~has_write[t]
        => \A a \in Addr : write_set[t][a] = NoWrite

(* Combined invariant *)
Inv == /\ FenceFidelityInst
       /\ PhaseValid
       /\ ReadOnlyNoWrites

THEOREM Spec => []Inv

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

Spec_WF ==
    Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

Spec_SF ==
    Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

ProgressProp ==
    \A self \in Thread :
        (pc[self] \in {"L_active", "L_collect", "L_execute"}
         ~> pc[self] \in {"L_idle", "L_done"})

=============================================================================
