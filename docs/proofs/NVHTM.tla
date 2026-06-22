------------------------ MODULE NVHTM ------------------------
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

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    MaxRetries          (* Max TSX retries before SGL fallback *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxRetries \in Nat \ {0}

VARIABLES
    mem,                (* [Addr -> Data] *)
    sgl,                (* 0 = free, t = held by thread t *)
    tsx_mode,           (* [Thread -> BOOLEAN] *)
    pc,                 (* [Thread -> {"idle", "active_tsx", "active_sgl",
                                      "tsx_commit", "flush_log", "write_cp",
                                      "apply_log", "clear_cp", "aborting"}] *)
    retry_cnt,          (* [Thread -> Nat] *)
    redo_log,           (* [Thread -> Seq(<<Addr, Data>>)]  per-TX redo log *)
    cp_valid,           (* [Thread -> BOOLEAN]  checkpoint marker *)
    cp_addr,            (* [Thread -> Nat]      which log buffer is checkpointed *)
    checkpoint,         (* [Thread -> BOOLEAN]  TRUE if checkpoint is active *)
    committed,          (* [Thread -> Nat]  commit count *)
    aborted             (* [Thread -> Nat]  abort count *)

vars == <<mem, sgl, tsx_mode, pc, retry_cnt, redo_log,
          cp_valid, cp_addr, checkpoint, committed, aborted>>

(*--------------------------------------------------------------------*)
(* Helpers                                                             *)
(*--------------------------------------------------------------------*)

NoWrite == 0 - 1

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ mem = [a \in Addr |-> 0]
    /\ sgl = 0
    /\ tsx_mode = [t \in Thread |-> FALSE]
    /\ pc = [t \in Thread |-> "idle"]
    /\ retry_cnt = [t \in Thread |-> 0]
    /\ redo_log = [t \in Thread |-> << >>]
    /\ cp_valid = [t \in Thread |-> FALSE]
    /\ cp_addr = [t \in Thread |-> 0]
    /\ checkpoint = [t \in Thread |-> FALSE]
    /\ committed = [t \in Thread |-> 0]
    /\ aborted = [t \in Thread |-> 0]

(*--------------------------------------------------------------------*)
(* TSX Path Actions                                                    *)
(*--------------------------------------------------------------------*)

(*── TSX Begin: _xbegin() ───────────────────────────────────────────*)
TSXBegin(t) ==
    /\ pc[t] \in {"idle", "active_sgl"}
    /\ sgl = 0
    /\ pc' = [pc EXCEPT ![t] = "active_tsx"]
    /\ tsx_mode' = [tsx_mode EXCEPT ![t] = TRUE]
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = 0]
    /\ redo_log' = [redo_log EXCEPT ![t] = << >>]  (* fresh log per TX *)
    /\ UNCHANGED <<mem, sgl, cp_valid, cp_addr, checkpoint,
                   committed, aborted>>

(*── TSX Read: RTM hardware tracks read-set ─────────────────────────*)
TSXRead(t, a) ==
    /\ pc[t] = "active_tsx"
    /\ a \in Addr
    /\ UNCHANGED vars

(*── TSX Write: append to redo log only ─────────────────────────────*)
(*  Writes go to the redo log (inside NVM).  Primary memory is NOT
    updated until the durable phase after _xend().  The RTM hardware
    tracks the log buffer address in its write-set. *)
TSXWrite(t, a, v) ==
    /\ pc[t] = "active_tsx"
    /\ a \in Addr
    /\ v \in Data
    /\ redo_log' = [redo_log EXCEPT ![t] = Append(redo_log[t], <<a, v>>)]
    (* mem is NOT changed — RTM will discard on abort *\)
    /\ UNCHANGED <<mem, sgl, tsx_mode, pc, retry_cnt,
                   cp_valid, cp_addr, checkpoint,
                   committed, aborted>>

(*── TSX Commit: _xend() → flush → checkpoint → apply ──────────────*)
(* Step 1: _xend() — end HTM transaction (atomic visibility of log) *)
TSXCommit(t) ==
    /\ pc[t] = "active_tsx"
    /\ tsx_mode' = [tsx_mode EXCEPT ![t] = FALSE]
    /\ pc' = [pc EXCEPT ![t] = "flush_log"]
    (* After _xend(), the redo log is atomically visible to
       other threads.  We proceed to the durable phase. *\)
    /\ UNCHANGED <<mem, sgl, retry_cnt, redo_log, cp_valid,
                   cp_addr, checkpoint, committed, aborted>>

(* Step 2: Flush redo log to NVM ────────────────────────────────────*)
FlushLog(t) ==
    /\ pc[t] = "flush_log"
    (* clwb all redo-log cachelines + sfence *\)
    /\ pc' = [pc EXCEPT ![t] = "write_cp"]
    /\ UNCHANGED <<mem, sgl, tsx_mode, retry_cnt, redo_log,
                   cp_valid, cp_addr, checkpoint, committed, aborted>>

(* Step 3: Write checkpoint ─────────────────────────────────────────*)
WriteCheckpoint(t) ==
    /\ pc[t] = "write_cp"
    (* Mark checkpoint: indicates redo log is durable *\)
    /\ cp_valid' = [cp_valid EXCEPT ![t] = TRUE]
    (* cp_addr stores the logical address of the redo log.
       For modeling, we use the thread ID as the key. *\)
    /\ cp_addr' = [cp_addr EXCEPT ![t] = t]
    /\ checkpoint' = [checkpoint EXCEPT ![t] = TRUE]
    /\ pc' = [pc EXCEPT ![t] = "apply_log"]
    /\ UNCHANGED <<mem, sgl, tsx_mode, retry_cnt, redo_log,
                   committed, aborted>>

(* Step 4: Apply redo log to primary memory ─────────────────────────*)
ApplyLog(t) ==
    /\ pc[t] = "apply_log"
    (* Write each redo-log entry to its final address *\)
    /\ LET ApplyOne(m, i) ==
           [a \in Addr |->
               IF i <= Len(redo_log[t]) /\ redo_log[t][i][1] = a
               THEN redo_log[t][i][2]
               ELSE m[a]]
       IN
       (* Apply all entries sequentially (fold) *\)
       /\ mem' = [a \in Addr |->
                    IF \E i \in 1..Len(redo_log[t]) :
                         redo_log[t][i][1] = a
                    THEN
                        (* Find the last write to this address in the log *\)
                        LET LastIdx ==
                            CHOOSE i \in 1..Len(redo_log[t]) :
                                redo_log[t][i][1] = a /\
                                \A j \in 1..Len(redo_log[t]) :
                                    redo_log[t][j][1] = a => j <= i
                        IN redo_log[t][LastIdx][2]
                    ELSE mem[a]]
    /\ pc' = [pc EXCEPT ![t] = "clear_cp"]
    /\ UNCHANGED <<sgl, tsx_mode, retry_cnt, redo_log,
                   cp_valid, cp_addr, checkpoint, committed, aborted>>

(* Step 5: Clear checkpoint ─────────────────────────────────────────*)
ClearCheckpoint(t) ==
    /\ pc[t] = "clear_cp"
    /\ cp_valid' = [cp_valid EXCEPT ![t] = FALSE]
    /\ checkpoint' = [checkpoint EXCEPT ![t] = FALSE]
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, sgl, tsx_mode, retry_cnt, redo_log,
                   cp_addr, aborted>>

(*── TSX Abort (_xabort): discard redo log ──────────────────────────*)
TSXAbort(t) ==
    /\ pc[t] = "active_tsx"
    (* HTM hardware discards all writes (including log entries).
       The redo log from this aborted TX is stale — discard it. *\)
    /\ redo_log' = [redo_log EXCEPT ![t] = << >>]
    /\ tsx_mode' = [tsx_mode EXCEPT ![t] = FALSE]
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = retry_cnt[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ UNCHANGED <<mem, sgl, cp_valid, cp_addr, checkpoint,
                   committed, aborted>>

(*── After abort: retry or fall back to SGL ─────────────────────────*)
TSXRetryOrFallback(t) ==
    /\ pc[t] = "aborting"
    /\ IF retry_cnt[t] < MaxRetries
       THEN
           /\ pc' = [pc EXCEPT ![t] = "active_tsx"]
           /\ tsx_mode' = [tsx_mode EXCEPT ![t] = TRUE]
           /\ redo_log' = [redo_log EXCEPT ![t] = << >>]
       ELSE
           /\ pc' = [pc EXCEPT ![t] = "active_sgl"]
           /\ UNCHANGED <<tsx_mode, redo_log>>
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ UNCHANGED <<mem, sgl, retry_cnt, cp_valid, cp_addr,
                   checkpoint, committed>>

(*--------------------------------------------------------------------*)
(* SGL Path Actions                                                    *)
(*--------------------------------------------------------------------*)

(*── SGL Begin: acquire mutex ───────────────────────────────────────*)
SGLBegin(t) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = 0
    /\ sgl' = t
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = 0]
    /\ redo_log' = [redo_log EXCEPT ![t] = << >>]
    /\ UNCHANGED <<mem, tsx_mode, pc, cp_valid, cp_addr,
                   checkpoint, committed, aborted>>

(*── SGL Read ───────────────────────────────────────────────────────*)
SGLRead(t, a) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = t
    /\ a \in Addr
    /\ UNCHANGED vars

(*── SGL Write: directly to memory + redo log for durability ───────*)
SGLWrite(t, a, v) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = t
    /\ a \in Addr
    /\ v \in Data
    (* In SGL mode, the mutex guarantees isolation, so we can
       write directly to memory.  We also log for recovery. *\)
    /\ redo_log' = [redo_log EXCEPT ![t] = Append(redo_log[t], <<a, v>>)]
    /\ mem' = [mem EXCEPT ![a] = v]
    /\ UNCHANGED <<sgl, tsx_mode, pc, retry_cnt,
                   cp_valid, cp_addr, checkpoint,
                   committed, aborted>>

(*── SGL Commit: full durable path ──────────────────────────────────*)
SGLCommit(t) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = t
    (* Same durable path as TSX: flush → checkpoint → apply *\)
    /\ sgl' = 0
    /\ pc' = [pc EXCEPT ![t] = "flush_log"]
    (* In SGL mode we skip _xend() and go directly to flush *\)
    /\ UNCHANGED <<mem, tsx_mode, retry_cnt, redo_log, cp_valid,
                   cp_addr, checkpoint, committed, aborted>>

(*--------------------------------------------------------------------*)
(* Recovery                                                            *)
(*--------------------------------------------------------------------*)

(*── Recovery: replay checkpointed redo log ─────────────────────────*)
Recovery(t) ==
    (* A checkpoint exists for thread t *\)
    /\ checkpoint[t] = TRUE
    /\ cp_valid[t] = TRUE
    (* Replay the redo log *\)
    /\ LET LogLen == Len(redo_log[t]) IN
       LogLen > 0 =>
         /\ mem' = [a \in Addr |->
                      IF \E i \in 1..LogLen : redo_log[t][i][1] = a
                      THEN redo_log[t][i][2]
                      ELSE mem[a]]
    (* Clear the checkpoint after replay *\)
    /\ checkpoint' = [checkpoint EXCEPT ![t] = FALSE]
    /\ cp_valid' = [cp_valid EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<sgl, tsx_mode, pc, retry_cnt, redo_log,
                   cp_addr, committed, aborted>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \E t \in Thread :
        \/ TSXBegin(t)
        \/ \E a \in Addr : TSXRead(t, a)
        \/ \E a \in Addr : \E v \in Data : TSXWrite(t, a, v)
        \/ TSXCommit(t)
        \/ FlushLog(t)
        \/ WriteCheckpoint(t)
        \/ ApplyLog(t)
        \/ ClearCheckpoint(t)
        \/ TSXAbort(t)
        \/ TSXRetryOrFallback(t)
        \/ SGLBegin(t)
        \/ \E a \in Addr : SGLRead(t, a)
        \/ \E a \in Addr : \E v \in Data : SGLWrite(t, a, v)
        \/ SGLCommit(t)
        \/ Recovery(t)

Spec == Init /\ [][Next]_vars

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

(*── I3: SGL owner is in SGL mode ──────────────────────────────────*)
LockOwnerInv ==
    \A t \in Thread :
        sgl = t => pc[t] \in {"active_sgl", "flush_log", "write_cp",
                                "apply_log", "clear_cp"}

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
        pc[t] = "idle" => checkpoint[t] = FALSE

(*── I7: Re-do log is empty at transaction start ───────────────────*)
FreshLogOnBegin ==
    \A t \in Thread :
        (pc[t] \in {"active_tsx", "active_sgl"}) =>
            (\A a \in Addr :
                ~ \E i \in 1..Len(redo_log[t]) : redo_log[t][i][1] = a)

(*── I8: No thread in intermediate commit states after checkpoint ──*)
CommitPhaseOrdering ==
    \A t \in Thread :
        \/ pc[t] \notin {"flush_log", "write_cp", "apply_log", "clear_cp"}
        \/ checkpoint[t] = TRUE

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

(* Every started transaction eventually completes *)
Completion ==
    \A t \in Thread :
        (pc[t] \in {"active_tsx", "active_sgl"})
        ~> (pc[t] = "idle")

(* After recovery, all checkpointed state is applied *)
RecoveryCompletes ==
    \A t \in Thread :
        (checkpoint[t] = TRUE) ~> (checkpoint[t] = FALSE)

(*====================================================================*)
(* Model parameters                                                   *)
(*====================================================================*)

(* Default: Thread = {1, 2}; Addr = {0, 1}; Data = {0, 1};
   MaxRetries = 2 *)

====
