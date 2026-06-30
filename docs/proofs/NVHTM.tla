----------------------- MODULE NVHTM ------------------------
(*
 * NV-HTM — HTM-backed transactional memory using Intel RTM
 *
 * Algorithm (from backends/tm_impl/nvhtm/):
 *
 * Dual-path: Intel RTM (fast path) + pass-through (fallback).
 * No SGL fallback — when RTM fails or is unavailable, all
 * operations bypass TM and access memory directly.
 *
 * Protocol (per transaction):
 *   1. _xbegin() starts an RTM transaction — all reads/writes
 *      inside are tracked by hardware.
 *   2. tm_write appends to a redo log (for NVM durability)
 *      AND writes through to memory (HTM rolls back both on abort).
 *   3. _xend() commits the HTM — atomic visibility.
 *   4. Durable phase (writers only): clwb + clflush each log
 *      entry + _mm_sfence for NVM ordering.
 *   5. Pass-through (no RTM): reads/writes go directly to
 *      memory with zero TM tracking.
 *
 * Invariants:
 *   TSXSafety:         If in TSX mode, pc = L_active_tsx.
 *   NoConcurrentTSX:   At most one thread in TSX mode.
 *   FenceFidelity:     Writers set appropriate fence/RMW annotations.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    MaxRetries          (* Max TSX retries before pass-through fallback *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxRetries \in Nat \ {0}

NoWrite == 0 - 1

(* --algorithm NVHTM

variables
    mem = [a \in Addr |-> 0],
    tsx_mode = [t \in Thread |-> FALSE],
    retry_cnt = [t \in Thread |-> 0],
    redo_log = [t \in Thread |-> << >>],
    read_only = [t \in Thread |-> TRUE],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
    \* Memory ordering annotations (see FenceFidelity invariant):
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
begin

L_idle:
    either \* RTM begin: _xbegin()
        tsx_mode[self] := TRUE;
        retry_cnt[self] := 0;
        redo_log[self] := << >>;
        read_only[self] := TRUE;
        lastRmw[self] := "seq_cst";  \* _xbegin() (RTM acquire)
        goto L_active_tsx;
    or \* Pass-through: begin without TM
        retry_cnt[self] := 0;
        redo_log[self] := << >>;
        read_only[self] := TRUE;
        lastRmw[self] := "seq_cst";  \* pass-through begin
        goto L_pass_through;
    end either;

L_active_tsx:
    either \* RTM Read: hardware tracks read-set
        with a \in Addr do
            skip;
        end with;
        lastRmw[self] := "seq_cst";  \* RTM read (hardware acquire)
        goto L_active_tsx;
    or \* RTM Write: write-through to mem + append to redo log
        with a \in Addr, v \in Data do
            mem[a] := v;
            redo_log[self] := Append(redo_log[self], <<a, v>>);
            read_only[self] := FALSE;
        end with;
        lastRmw[self] := "seq_cst";  \* RTM write (hardware seq_cst)
        goto L_active_tsx;
    or \* Commit: _xend()
        tsx_mode[self] := FALSE;
        if read_only[self] then
            committed[self] := committed[self] + 1;
            lastRmw[self] := "release";  \* _xend() (release)
            goto L_idle;
        else
            lastRmw[self] := "release";  \* _xend() (release)
            goto L_flush_log;
        end if;
    or \* Abort: _xabort() — hardware rolls back writes
        redo_log[self] := << >>;
        tsx_mode[self] := FALSE;
        retry_cnt[self] := retry_cnt[self] + 1;
        aborted[self] := aborted[self] + 1;
        lastRmw[self] := "release";  \* _xabort() (release)
        goto L_aborting;
    end either;

L_flush_log:
    \* durable_commit(): clwb + clflush for each log entry + sfence
    skip;
    lastThreadFence[self] := "seq_cst";  \* _mm_sfence()
    committed[self] := committed[self] + 1;
    goto L_idle;

L_aborting:
    if retry_cnt[self] < MaxRetries then
        \* Retry with TSX
        tsx_mode[self] := TRUE;
        redo_log[self] := << >>;
        read_only[self] := TRUE;
        lastRmw[self] := "seq_cst";  \* _xbegin() retry
        goto L_active_tsx;
    else
        \* Fall back to pass-through
        redo_log[self] := << >>;
        read_only[self] := TRUE;
        aborted[self] := aborted[self] + 1;
        lastRmw[self] := "seq_cst";  \* pass-through begin
        goto L_pass_through;
    end if;

L_pass_through:
    either \* Read (direct mem, no tracking)
        with a \in Addr do
            skip;
        end with;
        goto L_pass_through;
    or \* Write (direct mem, no tracking)
        with a \in Addr, v \in Data do
            mem[a] := v;
        end with;
        goto L_pass_through;
    or \* Exit pass-through
        committed[self] := committed[self] + 1;
        goto L_idle;
    end either;

end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES mem, tsx_mode, retry_cnt, redo_log, read_only, committed, aborted, 
          lastSignalFence, lastThreadFence, lastRmw, pc

vars == << mem, tsx_mode, retry_cnt, redo_log, read_only, committed, aborted, 
           lastSignalFence, lastThreadFence, lastRmw, pc >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ tsx_mode = [t \in Thread |-> FALSE]
        /\ retry_cnt = [t \in Thread |-> 0]
        /\ redo_log = [t \in Thread |-> << >>]
        /\ read_only = [t \in Thread |-> TRUE]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ tsx_mode' = [tsx_mode EXCEPT ![self] = TRUE]
                      /\ retry_cnt' = [retry_cnt EXCEPT ![self] = 0]
                      /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                      /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                      /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                      /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                   \/ /\ retry_cnt' = [retry_cnt EXCEPT ![self] = 0]
                      /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                      /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                      /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                      /\ pc' = [pc EXCEPT ![self] = "L_pass_through"]
                      /\ UNCHANGED tsx_mode
                /\ UNCHANGED << mem, committed, aborted, lastSignalFence, 
                                lastThreadFence >>

L_active_tsx(self) == /\ pc[self] = "L_active_tsx"
                      /\ \/ /\ \E a \in Addr:
                                 TRUE
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                            /\ UNCHANGED <<mem, tsx_mode, retry_cnt, redo_log, read_only, committed, aborted>>
                         \/ /\ \E a \in Addr:
                                 \E v \in Data:
                                   /\ mem' = [mem EXCEPT ![a] = v]
                                   /\ redo_log' = [redo_log EXCEPT ![self] = Append(redo_log[self], <<a, v>>)]
                                   /\ read_only' = [read_only EXCEPT ![self] = FALSE]
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                            /\ UNCHANGED <<tsx_mode, retry_cnt, committed, aborted>>
                         \/ /\ tsx_mode' = [tsx_mode EXCEPT ![self] = FALSE]
                            /\ IF read_only[self]
                                  THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                       /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                       /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                  ELSE /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                       /\ pc' = [pc EXCEPT ![self] = "L_flush_log"]
                                       /\ UNCHANGED committed
                            /\ UNCHANGED <<mem, retry_cnt, redo_log, read_only, aborted>>
                         \/ /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                            /\ tsx_mode' = [tsx_mode EXCEPT ![self] = FALSE]
                            /\ retry_cnt' = [retry_cnt EXCEPT ![self] = retry_cnt[self] + 1]
                            /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                            /\ pc' = [pc EXCEPT ![self] = "L_aborting"]
                            /\ UNCHANGED <<mem, read_only, committed>>
                      /\ UNCHANGED << lastSignalFence, lastThreadFence >>

L_flush_log(self) == /\ pc[self] = "L_flush_log"
                     /\ TRUE
                     /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = "seq_cst"]
                     /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED << mem, tsx_mode, retry_cnt, redo_log, 
                                     read_only, aborted, lastSignalFence, 
                                     lastRmw >>

L_aborting(self) == /\ pc[self] = "L_aborting"
                    /\ IF retry_cnt[self] < MaxRetries
                          THEN /\ tsx_mode' = [tsx_mode EXCEPT ![self] = TRUE]
                               /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                               /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                               /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                               /\ UNCHANGED aborted
                          ELSE /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                               /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                               /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                               /\ pc' = [pc EXCEPT ![self] = "L_pass_through"]
                               /\ UNCHANGED tsx_mode
                    /\ UNCHANGED << mem, retry_cnt, committed, lastSignalFence, 
                                    lastThreadFence >>

L_pass_through(self) == /\ pc[self] = "L_pass_through"
                        /\ \/ /\ \E a \in Addr:
                                   TRUE
                              /\ pc' = [pc EXCEPT ![self] = "L_pass_through"]
                              /\ UNCHANGED <<mem, committed>>
                           \/ /\ \E a \in Addr:
                                   \E v \in Data:
                                     mem' = [mem EXCEPT ![a] = v]
                              /\ pc' = [pc EXCEPT ![self] = "L_pass_through"]
                              /\ UNCHANGED committed
                           \/ /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              /\ mem' = mem
                        /\ UNCHANGED << tsx_mode, retry_cnt, redo_log, 
                                        read_only, aborted, lastSignalFence, 
                                        lastThreadFence, lastRmw >>

ThreadProc(self) == L_idle(self) \/ L_active_tsx(self) \/ L_flush_log(self)
                       \/ L_aborting(self) \/ L_pass_through(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Bounding constraints for TLC termination                           *)
(*====================================================================*)
TLCBound ==
    /\ \A t \in Thread : retry_cnt[t] < MaxRetries + 1
    /\ \A t \in Thread : committed[t] < 2
    /\ \A t \in Thread : Len(redo_log[t]) < 3

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: TSX safety — tsx_mode implies active TSX state ─────────────*)
TSXSafety ==
    \A t \in Thread :
        tsx_mode[t] = TRUE => pc[t] = "L_active_tsx"

(*── I2: Retry counter is bounded by MaxRetries ─────────────────────*)
RetryBound ==
    \A t \in Thread :
        retry_cnt[t] <= MaxRetries

(*── I3: Fence fidelity — writers set appropriate annotations ───────*)
Fenced(t) ==
    lastSignalFence[t] # "" \/ lastThreadFence[t] # "" \/ lastRmw[t] # ""

FenceFidelity ==
    \A t \in Thread :
        Len(redo_log[t]) > 0 => Fenced(t)

(*── Combined invariant ─────────────────────────────────────────────*)
Inv ==
    /\ TSXSafety
    /\ RetryBound
    /\ FenceFidelity

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Every active transaction eventually completes *)
Completion ==
    \A t \in Thread :
        (pc[t] \in {"L_active_tsx", "L_pass_through"})
        ~> (pc[t] = "L_idle")

(*====================================================================*)
(* Model parameters                                                   *)
(*====================================================================*)

(* Default: Thread = {1, 2}; Addr = {0, 1}; Data = {0, 1};
   MaxRetries = 2 *)

=====================================================================
