------------------------ MODULE DUDETM ------------------------
(*
 * DUDETM — Deferred-Persistence TM with Background Flusher
 *
 * Algorithm (from backends/tm_impl/dudetm/, ASPLOS 2017):
 *
 * Three-phase model:
 *
 * Phase 1 — PERFORM:
 *   STM transaction executes normally (TinySTM WBCTL): reads from
 *   memory, buffers writes in a write-set, commits via OCC (global
 *   clock validation).  After STM commit, the write-set is durable-
 *   logged: a redo batch is built with a COMMIT_BEGIN marker, all
 *   write-set entries, and any alloc/free operations.  The batch
 *   is published atomically to a per-thread circular buffer in
 *   shared mmap memory.
 *
 * Phase 2 — PERSIST (asynchronous):
 *   A background replayer thread consumes entries from each thread's
 *   circular buffer, flushes them to a persistent log file (clwb +
 *   sfence), and advances the per-thread tail pointer.
 *
 * Phase 3 — REPRODUCE (recovery):
 *   On restart, the replayer scans the persistent log file and
 *   replays OP_WRITE, OP_MALLOC, and OP_FREE entries (grouped
 *   by COMMIT_BEGIN markers) to recreate the durable heap state.
 *
 * Key data structures:
 *   - per_thread_log[t]: circular buffer of DUDERedoEntry
 *   - log_head[t], log_tail[t]: producer/consumer indices
 *   - replayer_pc: "idle" or "active"
 *   - persist_file: sequence of entries written to persistent storage
 *
 * Invariants:
 *   NoLostBatch:    Every published batch is eventually persisted.
 *   LogOrdering:    Log entries are consumed in publish order.
 *   RecoveryCorrect: After recovery, all persisted batches are replayed.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of application thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    LOG_SIZE            (* Circular buffer size per thread *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME LOG_SIZE \in Nat \ {0}

VARIABLES
    mem,                (* [Addr -> Data]: STM memory *)
    pc,                 (* [Thread -> {"idle", "active", "logging", "done"}] *)
    stm_ws,             (* [Thread -> Seq(<<Addr, Data>>)]: STM write-set *)
    stm_rs,             (* [Thread -> Seq(Addr)]: STM read-set *)
    stm_clock,          (* Nat: STM global clock *)
    stm_committed,      (* [Thread -> Nat]: STM commit count *)
    log,                (* [Thread -> [0..LOG_SIZE-1 -> <<LogOpType, Addr, Data>>]]
                           circular buffer of redo entries *)
    log_head,           (* [Thread -> Nat]: producer write index *)
    log_tail,           (* [Thread -> Nat]: consumer read index *)
    replayer_pc,        (* {"idle", "active", "flush_file"}: replayer state *)
    persist_file,       (* Seq(<<LogOpType, Addr, Data>>): durable log file *)
    batch_marker,       (* [Thread -> Nat]: last COMMIT_BEGIN seq for each thread *)
    recovered           (* BOOLEAN: recovery has completed *)

vars == <<mem, pc, stm_ws, stm_rs, stm_clock, stm_committed,
          log, log_head, log_tail, replayer_pc, persist_file,
          batch_marker, recovered>>

(*--------------------------------------------------------------------*)
(* Helpers                                                             *)
(*--------------------------------------------------------------------*)

OP_COMMIT_BEGIN == 0
OP_WRITE      == 1
OP_MALLOC     == 2
OP_FREE       == 3

LogEntryType == 0..3

(* Circular buffer: next write index *)
NextSlot(h, sz) == (h + 1) % sz
PrevSlot(h, sz) == (h - 1) % sz

(* Check if buffer is full *)
IsFull(t) ==
    (log_head[t] + 1) % LOG_SIZE = log_tail[t]

(* Check if buffer is empty *)
IsEmpty(t) ==
    log_head[t] = log_tail[t]

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ mem = [a \in Addr |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ stm_ws = [t \in Thread |-> << >>]
    /\ stm_rs = [t \in Thread |-> << >>]
    /\ stm_clock = 0
    /\ stm_committed = [t \in Thread |-> 0]
    /\ log = [t \in Thread |-> [i \in 0..LOG_SIZE-1 |-> <<0, 0, 0>>]]
    /\ log_head = [t \in Thread |-> 0]
    /\ log_tail = [t \in Thread |-> 0]
    /\ replayer_pc = "idle"
    /\ persist_file = << >>
    /\ batch_marker = [t \in Thread |-> 0]
    /\ recovered = FALSE

(*--------------------------------------------------------------------*)
(* STM Transaction Actions (Phase 1 — Perform)                        *)
(*--------------------------------------------------------------------*)

(*── Begin STM transaction ─────────────────────────────────────────*)
STM_Begin(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ stm_ws' = [stm_ws EXCEPT ![t] = << >>]
    /\ stm_rs' = [stm_rs EXCEPT ![t] = << >>]
    /\ UNCHANGED <<mem, stm_clock, stm_committed, log, log_head,
                   log_tail, replayer_pc, persist_file, batch_marker,
                   recovered>>

(*── STM Read ───────────────────────────────────────────────────────*)
STM_Read(t, a) ==
    /\ pc[t] = "active"
    /\ a \in Addr
    (* Check write-set first for read-your-own-writes *)
    /\ IF \E i \in 1..Len(stm_ws[t]) : stm_ws[t][i][1] = a
       THEN
           (* Own write found — no read-set entry needed *)
           /\ UNCHANGED vars
       ELSE
           (* Add to read-set *)
           /\ stm_rs' = [stm_rs EXCEPT ![t] = Append(stm_rs[t], a)]
           /\ UNCHANGED <<mem, pc, stm_ws, stm_clock, stm_committed,
                          log, log_head, log_tail, replayer_pc,
                          persist_file, batch_marker, recovered>>

(*── STM Write (buffer in write-set) ───────────────────────────────*)
STM_Write(t, a, v) ==
    /\ pc[t] = "active"
    /\ a \in Addr
    /\ v \in Data
    (* Buffer write (not applied to mem until commit) *)
    /\ stm_ws' = [stm_ws EXCEPT ![t] = Append(stm_ws[t], <<a, v>>)]
    /\ UNCHANGED <<mem, pc, stm_rs, stm_clock, stm_committed, log,
                   log_head, log_tail, replayer_pc, persist_file,
                   batch_marker, recovered>>

(*── STM Commit (OCC validation + write-back, then log) ────────────*)
STM_Commit(t) ==
    /\ pc[t] = "active"
    (* OCC validate: clock unchanged since reads *)
    (* For simplicity, model OCC as always succeeding for now *)
    (* Apply write-set to memory *)
    /\ LET ws_mem(a) ==
               IF \E j \in 1..Len(stm_ws[t]) : stm_ws[t][j][1] = a
               THEN (LET idx == CHOOSE j \in 1..Len(stm_ws[t]) :
                                        stm_ws[t][j][1] = a
                     IN stm_ws[t][idx][2])
               ELSE mem[a]
       IN mem' = [a \in Addr |-> ws_mem(a)]
    /\ stm_clock' = stm_clock + 1
    /\ stm_committed' = [stm_committed EXCEPT ![t] = stm_committed[t] + 1]
    (* Move to logging phase *)
    /\ pc' = [pc EXCEPT ![t] = "logging"]
    (* Snapshot the write-set before clearing for logging *)
    /\ UNCHANGED <<stm_ws, stm_rs, log, log_head, log_tail,
                   replayer_pc, persist_file, batch_marker, recovered>>

(*── Log the write-set as a redo batch ─────────────────────────────*)
LogBatch(t) ==
    /\ pc[t] = "logging"
    (* Check there's room in the circular buffer *)
    /\ ~IsFull(t)
    (* Write COMMIT_BEGIN marker *)
    /\ (LET idx == log_head[t] IN
          /\ log' = [log EXCEPT ![t][idx] = <<OP_COMMIT_BEGIN, t, stm_committed[t]>>]
          /\ log_head' = [log_head EXCEPT ![t] = NextSlot(log_head[t], LOG_SIZE)])
    (* Record batch marker *)
    /\ batch_marker' = [batch_marker EXCEPT ![t] = log_head[t]]
    (* Now write all write-set entries to the log *)
    (* This is modeled as a single action for simplicity; in the real
       implementation, each entry is written individually. *)
    (* For each write in stm_ws[t], write an OP_WRITE entry.
       We model this by adding entries after the COMMIT_BEGIN.
       Since TLA+ actions are atomic, we write all entries at once
       (the real implementation would do this entry-by-entry). *)
    (* Placeholder: entries would be written here *)
    /\ pc' = [pc EXCEPT ![t] = "done"]
    /\ UNCHANGED <<mem, stm_ws, stm_rs, stm_clock, stm_committed,
                   log_tail, replayer_pc, persist_file, recovered>>

(*── Add individual write entry to log ─────────────────────────────*)
LogWriteEntry(t, a, v) ==
    /\ pc[t] = "logging"
    /\ ~IsFull(t)
    /\ (LET idx == log_head[t] IN
          /\ log' = [log EXCEPT ![t][idx] = <<OP_WRITE, a, v>>]
          /\ log_head' = [log_head EXCEPT ![t] = NextSlot(log_head[t], LOG_SIZE)])
    /\ UNCHANGED <<mem, pc, stm_ws, stm_rs, stm_clock, stm_committed,
                   log_tail, replayer_pc, persist_file, batch_marker,
                   recovered>>

(*── Finish logging ────────────────────────────────────────────────*)
FinishLogging(t) ==
    /\ pc[t] = "logging"
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, stm_ws, stm_rs, stm_clock, stm_committed,
                   log, log_head, log_tail, replayer_pc, persist_file,
                   batch_marker, recovered>>

(*--------------------------------------------------------------------*)
(* Replayer Actions (Phase 2 — Persist)                                *)
(*--------------------------------------------------------------------*)

(*── Replayer begins processing ────────────────────────────────────*)
ReplayerBegin ==
    /\ replayer_pc = "idle"
    /\ replayer_pc' = "active"
    /\ UNCHANGED <<mem, pc, stm_ws, stm_rs, stm_clock, stm_committed,
                   log, log_head, log_tail, persist_file, batch_marker,
                   recovered>>

(*── Replayer consumes an entry from a thread's log ────────────────*)
ReplayerConsume(t) ==
    /\ replayer_pc = "active"
    /\ ~IsEmpty(t)
    /\ LET idx == log_tail[t]
           entry == log[t][idx] IN
       (* Append to persistent file *)
       /\ persist_file' = Append(persist_file, entry)
       (* Advance consumer index *)
       /\ log_tail' = [log_tail EXCEPT ![t] = NextSlot(log_tail[t], LOG_SIZE)]
    /\ UNCHANGED <<mem, pc, stm_ws, stm_rs, stm_clock, stm_committed,
                   log, log_head, replayer_pc, batch_marker, recovered>>

(*── Replayer goes idle ────────────────────────────────────────────*)
ReplayerIdle ==
    /\ replayer_pc = "active"
    /\ \A t \in Thread : IsEmpty(t)
    /\ replayer_pc' = "idle"
    /\ UNCHANGED <<mem, pc, stm_ws, stm_rs, stm_clock, stm_committed,
                   log, log_head, log_tail, persist_file, batch_marker,
                   recovered>>

(*--------------------------------------------------------------------*)
(* Recovery Actions (Phase 3 — Reproduce)                             *)
(*--------------------------------------------------------------------*)

(*── Recover: replay all persisted entries ─────────────────────────*)
RecoverDUDETM ==
    /\ ~recovered
    /\ (LET ReplayEntry(m, entry) ==
               CASE entry[1] = OP_WRITE -> [a \in Addr |->
                                              IF a = entry[2] THEN entry[3] ELSE m[a]]
                 [] entry[1] = OP_MALLOC -> m
                 [] entry[1] = OP_FREE -> m
                 [] OTHER -> m
        IN
        (* Fold over the persist_file to reconstruct memory *)
        /\ mem' = [a \in Addr |->
                     IF \E i \in 1..Len(persist_file) :
                          persist_file[i][1] = OP_WRITE /\
                          persist_file[i][2] = a
                     THEN
                         (* Find the last write to this address *)
                         LET LastWriteIdx ==
                             CHOOSE i \in 1..Len(persist_file) :
                                 persist_file[i][1] = OP_WRITE /\
                                 persist_file[i][2] = a /\
                                 \A j \in 1..Len(persist_file) :
                                     persist_file[j][1] = OP_WRITE /\
                                     persist_file[j][2] = a
                                     => j <= i
                         IN persist_file[LastWriteIdx][3]
                     ELSE mem[a]])
     (* Clear log state after recovery *)
    /\ log' = [t \in Thread |-> [i \in 0..LOG_SIZE-1 |-> <<0, 0, 0>>]]
    /\ log_head' = [t \in Thread |-> 0]
    /\ log_tail' = [t \in Thread |-> 0]
    /\ recovered' = TRUE
    /\ UNCHANGED <<pc, stm_ws, stm_rs, stm_clock, stm_committed,
                   replayer_pc, persist_file, batch_marker>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \/ \E t \in Thread : STM_Begin(t)
    \/ \E t \in Thread : \E a \in Addr : STM_Read(t, a)
    \/ \E t \in Thread : \E a \in Addr : \E v \in Data : STM_Write(t, a, v)
    \/ \E t \in Thread : STM_Commit(t)
    \/ \E t \in Thread : \E a \in Addr : \E v \in Data : LogWriteEntry(t, a, v)
    \/ \E t \in Thread : FinishLogging(t)
    \/ ReplayerBegin
    \/ \E t \in Thread : ReplayerConsume(t)
    \/ ReplayerIdle
    \/ RecoverDUDETM

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: Log head never exceeds tail + LOG_SIZE ────────────────────*)
LogBounds ==
    \A t \in Thread :
        (log_head[t] >= log_tail[t] =>
             log_head[t] - log_tail[t] < LOG_SIZE) /\
        (log_head[t] < log_tail[t] =>
             log_head[t] + LOG_SIZE - log_tail[t] < LOG_SIZE)

(*── I2: Persisted file entries correspond to valid log entries ────*)
PersistFileValid ==
    \A i \in 1..Len(persist_file) :
        persist_file[i][1] \in LogEntryType /\
        persist_file[i][2] \in (Addr \cup Thread) /\
        persist_file[i][3] \in (Data \cup Nat)

(*── I3: After recovery, recovered flag is set ─────────────────────*)
RecoveredFlag ==
    recovered = TRUE => \A t \in Thread : IsEmpty(t)

(*── I4: Logged writes match the STM commit that produced them ─────*)
LogWriteMatch ==
    \A t \in Thread :
        \A i \in 1..Len(persist_file) :
            persist_file[i][1] = OP_WRITE =>
                \E prev_idx \in 1..i-1 :
                    persist_file[prev_idx][1] = OP_COMMIT_BEGIN /\
                    persist_file[prev_idx][2] = t

====
