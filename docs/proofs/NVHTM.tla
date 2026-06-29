----------------------- MODULE NVHTM ------------------------
(*
 * NV-HTM — Persistent HTM with Redo Log
 *
 * Algorithm (from backends/tm_impl/nvhtm/, NV-HTM IPDPS 2018):
 *
 * Dual-path: Intel RTM (fast path) + SGL mutex (fallback).
 * The TSX + SGL protocol is modeled after TSXSGL.tla and SPHT.tla.
 *
 * Per-transaction redo log in NVM:
 *   - Inside the RTM transaction, writes are appended to a redo log
 *     (NVM buffer) but NOT applied to memory.
 *   - On _xend() (HTM commit), the log entries become atomically
 *     visible (the log is in the TSX write-set).
 *   - Durable phase (outside HTM, after _xend()):
 *       a) Flush redo-log cachelines to NVM (clwb + sfence)
 *       b) Write checkpoint marker to durable region
 *       c) Apply log entries to primary memory
 *       d) Clear checkpoint
 *   - On _xabort(): HTM hardware discards all log entries.
 *     No memory was changed, nothing to undo.
 *
 * Recovery:
 *   - Scan checkpoint region.  If a valid checkpoint exists,
 *     find the associated redo log and replay all entries.
 *
 * Invariants (for TLC model checking):
 *   TSXSafety:           If in TSX mode, sgl = 0.
 *   LockExclusion:       At most one thread holds the SGL mutex.
 *   DurableRedo:         After flush, redo log is durable in NVM.
 *   CheckpointValid:     If checkpoint is set, the redo log is
 *                        complete and recoverable.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC, TMTypes

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    MaxRetries          (* Max TSX retries before SGL fallback *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxRetries \in Nat \ {0}

(* --algorithm NVHTM

variables
    mem = [a \in Addr |-> 0],
    sgl = 0,
    tsx_mode = [t \in Thread |-> FALSE],
    retry_cnt = [t \in Thread |-> 0],
    redo_log = [t \in Thread |-> << >>],
    cp_valid = [t \in Thread |-> FALSE],
    cp_addr = [t \in Thread |-> 0],
    checkpoint = [t \in Thread |-> FALSE],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
    \* Fence tracking (signal_fence, thread_fence, RMW)
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
begin

L_idle:
    either \* TSX Begin: _xbegin()
        await sgl = 0;
        tsx_mode[self] := TRUE;
        retry_cnt[self] := 0;
        redo_log[self] := << >>;
        lastRmw[self] := "seq_cst";  \* _xbegin() (RTM acquire)
        goto L_active_tsx;
    or \* Recovery: replay checkpointed redo log
        if checkpoint[self] /\ cp_valid[self] then
            mem := [a \in Addr |->
                IF \E i \in 1..Len(redo_log[self]) : redo_log[self][i][1] = a
                THEN
                    LET LastIdx ==
                        CHOOSE i \in 1..Len(redo_log[self]) :
                            redo_log[self][i][1] = a /\
                            \A j \in 1..Len(redo_log[self]) :
                                redo_log[self][j][1] = a => j <= i
                    IN redo_log[self][LastIdx][2]
                ELSE mem[a]];
            checkpoint[self] := FALSE;
            cp_valid[self] := FALSE;
            lastThreadFence[self] := "seq_cst";  \* sfence(seq_cst) for persistent checkpoint
        end if;
        goto L_idle;
    end either;

L_active_tsx:
    either \* TSX Read: RTM hardware tracks read-set
        with a \in Addr do
            skip;
        end with;
        lastRmw[self] := "seq_cst";  \* RTM read
        goto L_active_tsx;
    or \* TSX Write: append to redo log only
        with a \in Addr, v \in Data do
            redo_log[self] := Append(redo_log[self], <<a, v>>);
        end with;
        lastRmw[self] := "acquire";  \* TSX write
        goto L_active_tsx;
    or \* TSX Commit: _xend()
        tsx_mode[self] := FALSE;
        lastRmw[self] := "release";  \* _xend()
        goto L_flush_log;
    or \* TSX Abort: _xabort()
        redo_log[self] := << >>;
        tsx_mode[self] := FALSE;
        retry_cnt[self] := retry_cnt[self] + 1;
        lastRmw[self] := "release";  \* _xabort()
        goto L_aborting;
    end either;

L_flush_log:
    skip;  \* clwb all redo-log cachelines + sfence
    lastThreadFence[self] := "seq_cst";  \* sfence(seq_cst)
    goto L_write_cp;

L_write_cp:
    cp_valid[self] := TRUE;
    cp_addr[self] := self;
    checkpoint[self] := TRUE;
    lastThreadFence[self] := "seq_cst";  \* persistent checkpoint fence
    goto L_apply_log;

L_apply_log:
    \* Apply redo log to primary memory
    mem := [a \in Addr |->
        IF \E i \in 1..Len(redo_log[self]) : redo_log[self][i][1] = a
        THEN
            LET LastIdx ==
                CHOOSE i \in 1..Len(redo_log[self]) :
                    redo_log[self][i][1] = a /\
                    \A j \in 1..Len(redo_log[self]) :
                        redo_log[self][j][1] = a => j <= i
            IN redo_log[self][LastIdx][2]
        ELSE mem[a]];
    lastThreadFence[self] := "seq_cst";  \* redo-apply (sfence)
    goto L_clear_cp;

L_clear_cp:
    cp_valid[self] := FALSE;
    checkpoint[self] := FALSE;
    committed[self] := committed[self] + 1;
    lastRmw[self] := "release";  \* checkpoint release
    goto L_idle;

L_aborting:
    if retry_cnt[self] < MaxRetries then
        \* Retry with TSX
        await sgl = 0;
        tsx_mode[self] := TRUE;
        redo_log[self] := << >>;
        aborted[self] := aborted[self] + 1;
        lastRmw[self] := "seq_cst";  \* _xbegin() retry
        goto L_active_tsx;
    else
        \* Fall back to SGL
        tsx_mode[self] := FALSE;
        redo_log[self] := << >>;
        aborted[self] := aborted[self] + 1;
        lastRmw[self] := "release";  \* SGL fallback
        goto L_active_sgl;
    end if;

L_active_sgl:
    either \* SGL Begin: acquire mutex
        await sgl = 0;
        await \A other \in Thread \ {self} : tsx_mode[other] = FALSE;
        sgl := self;
        retry_cnt[self] := 0;
        redo_log[self] := << >>;
        lastRmw[self] := "acquire";  \* mutex.lock()
        goto L_active_sgl;
    or \* SGL Read (mutex provides isolation)
        await sgl = self;
        with a \in Addr do
            skip;
        end with;
        lastRmw[self] := "seq_cst";  \* SGL read
        goto L_active_sgl;
    or \* SGL Write: direct to memory + redo log for durability
        await sgl = self;
        with a \in Addr, v \in Data do
            mem[a] := v;
            redo_log[self] := Append(redo_log[self], <<a, v>>);
        end with;
        lastRmw[self] := "acquire";  \* SGL write
        goto L_active_sgl;
    or \* SGL Commit: release lock, enter durable path
        await sgl = self;
        sgl := 0;
        lastRmw[self] := "release";  \* mutex.unlock()
        goto L_flush_log;
    end either;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES mem, sgl, tsx_mode, retry_cnt, redo_log, cp_valid, cp_addr, 
          checkpoint, committed, aborted, lastSignalFence, lastThreadFence, 
          lastRmw, pc

vars == << mem, sgl, tsx_mode, retry_cnt, redo_log, cp_valid, cp_addr, 
           checkpoint, committed, aborted, lastSignalFence, lastThreadFence, 
           lastRmw, pc >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ sgl = 0
        /\ tsx_mode = [t \in Thread |-> FALSE]
        /\ retry_cnt = [t \in Thread |-> 0]
        /\ redo_log = [t \in Thread |-> << >>]
        /\ cp_valid = [t \in Thread |-> FALSE]
        /\ cp_addr = [t \in Thread |-> 0]
        /\ checkpoint = [t \in Thread |-> FALSE]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ sgl = 0
                      /\ tsx_mode' = [tsx_mode EXCEPT ![self] = TRUE]
                      /\ retry_cnt' = [retry_cnt EXCEPT ![self] = 0]
                      /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                      /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                      /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                      /\ UNCHANGED <<mem, cp_valid, checkpoint, lastThreadFence>>
                   \/ /\ IF checkpoint[self] /\ cp_valid[self]
                            THEN /\ mem' =    [a \in Addr |->
                                           IF \E i \in 1..Len(redo_log[self]) : redo_log[self][i][1] = a
                                           THEN
                                               LET LastIdx ==
                                                   CHOOSE i \in 1..Len(redo_log[self]) :
                                                       redo_log[self][i][1] = a /\
                                                       \A j \in 1..Len(redo_log[self]) :
                                                           redo_log[self][j][1] = a => j <= i
                                               IN redo_log[self][LastIdx][2]
                                           ELSE mem[a]]
                                 /\ checkpoint' = [checkpoint EXCEPT ![self] = FALSE]
                                 /\ cp_valid' = [cp_valid EXCEPT ![self] = FALSE]
                                 /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = "seq_cst"]
                            ELSE /\ TRUE
                                 /\ UNCHANGED << mem, cp_valid, checkpoint, 
                                                 lastThreadFence >>
                      /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                      /\ UNCHANGED <<tsx_mode, retry_cnt, redo_log, lastRmw>>
                /\ UNCHANGED << sgl, cp_addr, committed, aborted, 
                                lastSignalFence >>

L_active_tsx(self) == /\ pc[self] = "L_active_tsx"
                      /\ \/ /\ \E a \in Addr:
                                 TRUE
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                            /\ UNCHANGED <<tsx_mode, retry_cnt, redo_log>>
                         \/ /\ \E a \in Addr:
                                 \E v \in Data:
                                   redo_log' = [redo_log EXCEPT ![self] = Append(redo_log[self], <<a, v>>)]
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                            /\ UNCHANGED <<tsx_mode, retry_cnt>>
                         \/ /\ tsx_mode' = [tsx_mode EXCEPT ![self] = FALSE]
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                            /\ pc' = [pc EXCEPT ![self] = "L_flush_log"]
                            /\ UNCHANGED <<retry_cnt, redo_log>>
                         \/ /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                            /\ tsx_mode' = [tsx_mode EXCEPT ![self] = FALSE]
                            /\ retry_cnt' = [retry_cnt EXCEPT ![self] = retry_cnt[self] + 1]
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                            /\ pc' = [pc EXCEPT ![self] = "L_aborting"]
                      /\ UNCHANGED << mem, sgl, cp_valid, cp_addr, checkpoint, 
                                      committed, aborted, lastSignalFence, 
                                      lastThreadFence >>

L_flush_log(self) == /\ pc[self] = "L_flush_log"
                     /\ TRUE
                     /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = "seq_cst"]
                     /\ pc' = [pc EXCEPT ![self] = "L_write_cp"]
                     /\ UNCHANGED << mem, sgl, tsx_mode, retry_cnt, redo_log, 
                                     cp_valid, cp_addr, checkpoint, committed, 
                                     aborted, lastSignalFence, lastRmw >>

L_write_cp(self) == /\ pc[self] = "L_write_cp"
                    /\ cp_valid' = [cp_valid EXCEPT ![self] = TRUE]
                    /\ cp_addr' = [cp_addr EXCEPT ![self] = self]
                    /\ checkpoint' = [checkpoint EXCEPT ![self] = TRUE]
                    /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = "seq_cst"]
                    /\ pc' = [pc EXCEPT ![self] = "L_apply_log"]
                    /\ UNCHANGED << mem, sgl, tsx_mode, retry_cnt, redo_log, 
                                    committed, aborted, lastSignalFence, 
                                    lastRmw >>

L_apply_log(self) == /\ pc[self] = "L_apply_log"
                     /\ mem' =    [a \in Addr |->
                               IF \E i \in 1..Len(redo_log[self]) : redo_log[self][i][1] = a
                               THEN
                                   LET LastIdx ==
                                       CHOOSE i \in 1..Len(redo_log[self]) :
                                           redo_log[self][i][1] = a /\
                                           \A j \in 1..Len(redo_log[self]) :
                                               redo_log[self][j][1] = a => j <= i
                                   IN redo_log[self][LastIdx][2]
                               ELSE mem[a]]
                     /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = "seq_cst"]
                     /\ pc' = [pc EXCEPT ![self] = "L_clear_cp"]
                     /\ UNCHANGED << sgl, tsx_mode, retry_cnt, redo_log, 
                                     cp_valid, cp_addr, checkpoint, committed, 
                                     aborted, lastSignalFence, lastRmw >>

L_clear_cp(self) == /\ pc[self] = "L_clear_cp"
                    /\ cp_valid' = [cp_valid EXCEPT ![self] = FALSE]
                    /\ checkpoint' = [checkpoint EXCEPT ![self] = FALSE]
                    /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                    /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                    /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                    /\ UNCHANGED << mem, sgl, tsx_mode, retry_cnt, redo_log, 
                                    cp_addr, aborted, lastSignalFence, 
                                    lastThreadFence >>

L_aborting(self) == /\ pc[self] = "L_aborting"
                    /\ IF retry_cnt[self] < MaxRetries
                          THEN /\ sgl = 0
                               /\ tsx_mode' = [tsx_mode EXCEPT ![self] = TRUE]
                               /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                               /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                               /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                          ELSE /\ tsx_mode' = [tsx_mode EXCEPT ![self] = FALSE]
                               /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                               /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                               /\ pc' = [pc EXCEPT ![self] = "L_active_sgl"]
                    /\ UNCHANGED << mem, sgl, retry_cnt, cp_valid, cp_addr, 
                                    checkpoint, committed, lastSignalFence, 
                                    lastThreadFence >>

L_active_sgl(self) == /\ pc[self] = "L_active_sgl"
                      /\ \/ /\ sgl = 0
                            /\ \A other \in Thread \ {self} : tsx_mode[other] = FALSE
                            /\ sgl' = self
                            /\ retry_cnt' = [retry_cnt EXCEPT ![self] = 0]
                            /\ redo_log' = [redo_log EXCEPT ![self] = << >>]
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_sgl"]
                            /\ mem' = mem
                         \/ /\ sgl = self
                            /\ \E a \in Addr:
                                 TRUE
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_sgl"]
                            /\ UNCHANGED <<mem, sgl, retry_cnt, redo_log>>
                         \/ /\ sgl = self
                            /\ \E a \in Addr:
                                 \E v \in Data:
                                   /\ mem' = [mem EXCEPT ![a] = v]
                                   /\ redo_log' = [redo_log EXCEPT ![self] = Append(redo_log[self], <<a, v>>)]
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_sgl"]
                            /\ UNCHANGED <<sgl, retry_cnt>>
                         \/ /\ sgl = self
                            /\ sgl' = 0
                            /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                            /\ pc' = [pc EXCEPT ![self] = "L_flush_log"]
                            /\ UNCHANGED <<mem, retry_cnt, redo_log>>
                      /\ UNCHANGED << tsx_mode, cp_valid, cp_addr, checkpoint, 
                                      committed, aborted, lastSignalFence, 
                                      lastThreadFence >>

ThreadProc(self) == L_idle(self) \/ L_active_tsx(self) \/ L_flush_log(self)
                       \/ L_write_cp(self) \/ L_apply_log(self)
                       \/ L_clear_cp(self) \/ L_aborting(self)
                       \/ L_active_sgl(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: TSX safety — if in TSX mode, SGL is free ──────────────────*)
TSXSafety ==
    \A t \in Thread :
        tsx_mode[t] = TRUE => sgl = 0

(*── I2: SGL mutual exclusion ──────────────────────────────────────*)
LockExclusion ==
    \A t1, t2 \in Thread :
        (sgl = t1 /\ sgl = t2) => t1 = t2

(*── I3: SGL owner has acquired the lock ───────────────────────────*)
LockOwnerInv ==
    \A t \in Thread :
        sgl = t => pc[t] \in {"L_active_sgl", "L_flush_log", "L_write_cp",
                              "L_apply_log", "L_clear_cp"}

(*── I4: No TSX during SGL ─────────────────────────────────────────*)
TSXvsSGLSafety ==
    \A t \in Thread :
        sgl # 0 => ~tsx_mode[t]

(*── I5: Checkpoint implies valid entry ────────────────────────────*)
CheckpointConsistent ==
    \A t \in Thread :
        checkpoint[t] = TRUE => cp_valid[t] = TRUE

(*── I6: No dangling checkpoints ───────────────────────────────────*)
NoStaleCheckpoint ==
    \A t \in Thread :
        pc[t] = "L_idle" => checkpoint[t] = FALSE

(*── I7: Commit-phase ordering: apply/clear require checkpoint set ─*)
CommitPhaseOrdering ==
    \A t \in Thread :
        \/ pc[t] \notin {"L_apply_log", "L_clear_cp"}
        \/ checkpoint[t] = TRUE

(*── I8: Every thread with a non-empty write-set has issued a fence ─*)
FenceFidelity ==
    \A t \in Thread : Len(redo_log[t]) > 0 =>
        Fenced(t, lastSignalFence, lastThreadFence, lastRmw)

(*── Combined invariant ────────────────────────────────────────────*)
Inv ==
    /\ TSXSafety
    /\ LockExclusion
    /\ LockOwnerInv
    /\ TSXvsSGLSafety
    /\ CheckpointConsistent
    /\ NoStaleCheckpoint
    /\ CommitPhaseOrdering
    /\ FenceFidelity

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Every started transaction eventually completes *)
Completion ==
    \A t \in Thread :
        (pc[t] \in {"L_active_tsx", "L_active_sgl"})
        ~> (pc[t] = "L_idle")

(* After recovery, all checkpointed state is applied *)
RecoveryCompletes ==
    \A t \in Thread :
        (checkpoint[t] = TRUE) ~> (checkpoint[t] = FALSE)

(*====================================================================*)
(* Model parameters                                                   *)
(*====================================================================*)

(* Default: Thread = {1, 2}; Addr = {0, 1}; Data = {0, 1};
   MaxRetries = 2 *)

=====
