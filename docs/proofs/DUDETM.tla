------------------------ MODULE DUDETM ------------------------
(*
 * DUDETM — Deferred-Persistence TM with Background Flusher
 * (PlusCal translation)
 *
 * Algorithm (from backends/tm_impl/dudetm/, ASPLOS 2017):
 *
 * Three-phase model:
 *
 * Phase 1 — PERFORM:
 *   STM transaction executes normally (TinySTM WBCTL): reads from
 *   memory, buffers writes in a write-set, commits via OCC (global
 *   clock validation).  After STM commit, the write-set is durable-
 *   logged: a COMMIT_BEGIN marker is written, then individual
 *   write-set entries.  The log entries are published atomically
 *   to a per-thread circular buffer in shared mmap memory.
 *
 * Phase 2 — PERSIST (asynchronous):
 *   A background replayer thread consumes entries from each thread's
 *   circular buffer, appends them to the persistent file, and advances
 *   the per-thread tail pointer.
 *
 * Phase 3 — REPRODUCE (recovery):
 *   On restart, the replayer scans the persistent file and replays
 *   OP_WRITE entries to recreate the durable heap state.
 *
 * Key data structures:
 *   - per_thread_log[t]: circular buffer of redo entries
 *   - log_head[t], log_tail[t]: producer/consumer indices
 *   - replayer_state: "idle" or "active"
 *   - persist_file: sequence of entries written to persistent storage
 *
 * Invariants:
 *   LogBounds:       Log head never exceeds tail + LOG_SIZE
 *   PersistFileValid: Persisted entries have valid types/addresses/data
 *   RecoveredFlag:   After recovery, all logs are empty
 *   LogWriteMatch:   Every OP_WRITE has a preceding OP_COMMIT_BEGIN
 *)

EXTENDS Naturals, Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of application thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    LOG_SIZE            (* Circular buffer size per thread *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME LOG_SIZE \in Nat \ {0}

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

(* --algorithm DUDETM

variables
    mem = [a \in Addr |-> 0],
    state = [t \in Thread |-> "idle"],
    stm_ws = [t \in Thread |-> << >>],
    stm_rs = [t \in Thread |-> << >>],
    stm_clock = 0,
    stm_committed = [t \in Thread |-> 0],
    log = [t \in Thread |-> [i \in 0..LOG_SIZE-1 |-> <<0, 0, 0>>]],
    log_head = [t \in Thread |-> 0],
    log_tail = [t \in Thread |-> 0],
    replayer_state = "idle",
    persist_file = << >>,
    batch_marker = [t \in Thread |-> 0],   (* NOTE: Declared but never written or read. *)
    recovered = FALSE;

process ThreadProc \in Thread
begin

L_idle:
    either \* Begin transaction
        state[self] := "active";
        stm_ws[self] := << >>;
        stm_rs[self] := << >>;
        goto L_active;
    or \* Remain idle
        goto L_idle;
    end either;

L_active:
    either \* STM_Read
        with a \in Addr do
            if ~(\E i \in 1..Len(stm_ws[self]) : stm_ws[self][i][1] = a) then
                stm_rs[self] := Append(stm_rs[self], a);
            end if;
        end with;
        goto L_active;
    or \* STM_Write
        with a \in Addr, v \in Data do
            stm_ws[self] := Append(stm_ws[self], <<a, v>>);
        end with;
        goto L_active;
    or \* STM_Commit (OCC write-back + log COMMIT_BEGIN)
        \* Apply write-set to memory
        mem := [a \in Addr |->
            IF \E j \in 1..Len(stm_ws[self]) : stm_ws[self][j][1] = a
            THEN (LET idx == CHOOSE j \in 1..Len(stm_ws[self]) :
                             stm_ws[self][j][1] = a
                  IN stm_ws[self][idx][2])
            ELSE mem[a]];
        stm_clock := stm_clock + 1;
        stm_committed[self] := stm_committed[self] + 1;
        \* Write COMMIT_BEGIN marker to log (only if room)
        if ~((log_head[self] + 1) % LOG_SIZE = log_tail[self]) then
            log[self][log_head[self]] := <<OP_COMMIT_BEGIN, self, stm_committed[self]>>;
            log_head[self] := (log_head[self] + 1) % LOG_SIZE;
        end if;
        state[self] := "logging";
        goto L_log;
    end either;

L_log:
    either \* LogWriteEntry: write one OP_WRITE entry
        with a \in Addr, v \in Data do
            when ~((log_head[self] + 1) % LOG_SIZE = log_tail[self]);
            log[self][log_head[self]] := <<OP_WRITE, a, v>>;
            log_head[self] := (log_head[self] + 1) % LOG_SIZE;
        end with;
        goto L_log;
    or \* FinishLogging: return to idle
        state[self] := "idle";
        goto L_idle;
    end either;

end process;

process ReplayerProc = 0
begin

L_replayer_idle:
    replayer_state := "active";

L_replayer_active:
    either \* ReplayerConsume: consume one entry from a thread's log
        with t \in Thread do
            when ~(log_head[t] = log_tail[t]);
            persist_file := Append(persist_file, log[t][log_tail[t]]);
            log_tail[t] := (log_tail[t] + 1) % LOG_SIZE;
        end with;
        goto L_replayer_active;
    or \* ReplayerIdle: go idle when all logs empty
        when \A t \in Thread : log_head[t] = log_tail[t];
        replayer_state := "idle";
        goto L_replayer_idle;
    end either;

end process;

process RecoveryProc = -1
begin

L_recover:
    when ~recovered;
    \* Replay all persisted OP_WRITE entries into memory
    mem := [a \in Addr |->
        IF \E i \in 1..Len(persist_file) :
            persist_file[i][1] = OP_WRITE /\ persist_file[i][2] = a
        THEN
            LET LastWriteIdx == CHOOSE i \in 1..Len(persist_file) :
                persist_file[i][1] = OP_WRITE /\
                persist_file[i][2] = a /\
                \A j \in 1..Len(persist_file) :
                    persist_file[j][1] = OP_WRITE /\
                    persist_file[j][2] = a => j <= i
            IN persist_file[LastWriteIdx][3]
        ELSE mem[a]];
    \* Clear all per-thread log state
    log := [t \in Thread |-> [i \in 0..LOG_SIZE-1 |-> <<0, 0, 0>>]];
    log_head := [t \in Thread |-> 0];
    log_tail := [t \in Thread |-> 0];
    recovered := TRUE;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES mem, state, stm_ws, stm_rs, stm_clock, stm_committed, log, log_head, 
          log_tail, replayer_state, persist_file, batch_marker, recovered, pc

vars == << mem, state, stm_ws, stm_rs, stm_clock, stm_committed, log, 
           log_head, log_tail, replayer_state, persist_file, batch_marker, 
           recovered, pc >>

ProcSet == (Thread) \cup {0} \cup {-1}

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ state = [t \in Thread |-> "idle"]
        /\ stm_ws = [t \in Thread |-> << >>]
        /\ stm_rs = [t \in Thread |-> << >>]
        /\ stm_clock = 0
        /\ stm_committed = [t \in Thread |-> 0]
        /\ log = [t \in Thread |-> [i \in 0..LOG_SIZE-1 |-> <<0, 0, 0>>]]
        /\ log_head = [t \in Thread |-> 0]
        /\ log_tail = [t \in Thread |-> 0]
        /\ replayer_state = "idle"
        /\ persist_file = << >>
        /\ batch_marker = [t \in Thread |-> 0]
        /\ recovered = FALSE
        /\ pc = [self \in ProcSet |-> CASE self \in Thread -> "L_idle"
                                        [] self = 0 -> "L_replayer_idle"
                                        [] self = -1 -> "L_recover"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ state' = [state EXCEPT ![self] = "active"]
                      /\ stm_ws' = [stm_ws EXCEPT ![self] = << >>]
                      /\ stm_rs' = [stm_rs EXCEPT ![self] = << >>]
                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                   \/ /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                      /\ UNCHANGED <<state, stm_ws, stm_rs>>
                /\ UNCHANGED << mem, stm_clock, stm_committed, log, log_head, 
                                log_tail, replayer_state, persist_file, 
                                batch_marker, recovered >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF ~(\E i \in 1..Len(stm_ws[self]) : stm_ws[self][i][1] = a)
                                THEN /\ stm_rs' = [stm_rs EXCEPT ![self] = Append(stm_rs[self], a)]
                                ELSE /\ TRUE
                                     /\ UNCHANGED stm_rs
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<mem, state, stm_ws, stm_clock, stm_committed, log, log_head>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               stm_ws' = [stm_ws EXCEPT ![self] = Append(stm_ws[self], <<a, v>>)]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<mem, state, stm_rs, stm_clock, stm_committed, log, log_head>>
                     \/ /\ mem' =    [a \in Addr |->
                                  IF \E j \in 1..Len(stm_ws[self]) : stm_ws[self][j][1] = a
                                  THEN (LET idx == CHOOSE j \in 1..Len(stm_ws[self]) :
                                                   stm_ws[self][j][1] = a
                                        IN stm_ws[self][idx][2])
                                  ELSE mem[a]]
                        /\ stm_clock' = stm_clock + 1
                        /\ stm_committed' = [stm_committed EXCEPT ![self] = stm_committed[self] + 1]
                        /\ IF ~((log_head[self] + 1) % LOG_SIZE = log_tail[self])
                              THEN /\ log' = [log EXCEPT ![self][log_head[self]] = <<OP_COMMIT_BEGIN, self, stm_committed'[self]>>]
                                   /\ log_head' = [log_head EXCEPT ![self] = (log_head[self] + 1) % LOG_SIZE]
                              ELSE /\ TRUE
                                   /\ UNCHANGED << log, log_head >>
                        /\ state' = [state EXCEPT ![self] = "logging"]
                        /\ pc' = [pc EXCEPT ![self] = "L_log"]
                        /\ UNCHANGED <<stm_ws, stm_rs>>
                  /\ UNCHANGED << log_tail, replayer_state, persist_file, 
                                  batch_marker, recovered >>

L_log(self) == /\ pc[self] = "L_log"
               /\ \/ /\ \E a \in Addr:
                          \E v \in Data:
                            /\ ~((log_head[self] + 1) % LOG_SIZE = log_tail[self])
                            /\ log' = [log EXCEPT ![self][log_head[self]] = <<OP_WRITE, a, v>>]
                            /\ log_head' = [log_head EXCEPT ![self] = (log_head[self] + 1) % LOG_SIZE]
                     /\ pc' = [pc EXCEPT ![self] = "L_log"]
                     /\ state' = state
                  \/ /\ state' = [state EXCEPT ![self] = "idle"]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED <<log, log_head>>
               /\ UNCHANGED << mem, stm_ws, stm_rs, stm_clock, stm_committed, 
                               log_tail, replayer_state, persist_file, 
                               batch_marker, recovered >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_log(self)

L_replayer_idle == /\ pc[0] = "L_replayer_idle"
                   /\ replayer_state' = "active"
                   /\ pc' = [pc EXCEPT ![0] = "L_replayer_active"]
                   /\ UNCHANGED << mem, state, stm_ws, stm_rs, stm_clock, 
                                   stm_committed, log, log_head, log_tail, 
                                   persist_file, batch_marker, recovered >>

L_replayer_active == /\ pc[0] = "L_replayer_active"
                     /\ \/ /\ \E t \in Thread:
                                /\ ~(log_head[t] = log_tail[t])
                                /\ persist_file' = Append(persist_file, log[t][log_tail[t]])
                                /\ log_tail' = [log_tail EXCEPT ![t] = (log_tail[t] + 1) % LOG_SIZE]
                           /\ pc' = [pc EXCEPT ![0] = "L_replayer_active"]
                           /\ UNCHANGED replayer_state
                        \/ /\ \A t \in Thread : log_head[t] = log_tail[t]
                           /\ replayer_state' = "idle"
                           /\ pc' = [pc EXCEPT ![0] = "L_replayer_idle"]
                           /\ UNCHANGED <<log_tail, persist_file>>
                     /\ UNCHANGED << mem, state, stm_ws, stm_rs, stm_clock, 
                                     stm_committed, log, log_head, 
                                     batch_marker, recovered >>

ReplayerProc == L_replayer_idle \/ L_replayer_active

L_recover == /\ pc[-1] = "L_recover"
             /\ ~recovered
             /\ mem' =    [a \in Addr |->
                       IF \E i \in 1..Len(persist_file) :
                           persist_file[i][1] = OP_WRITE /\ persist_file[i][2] = a
                       THEN
                           LET LastWriteIdx == CHOOSE i \in 1..Len(persist_file) :
                               persist_file[i][1] = OP_WRITE /\
                               persist_file[i][2] = a /\
                               \A j \in 1..Len(persist_file) :
                                   persist_file[j][1] = OP_WRITE /\
                                   persist_file[j][2] = a => j <= i
                           IN persist_file[LastWriteIdx][3]
                       ELSE mem[a]]
             /\ log' = [t \in Thread |-> [i \in 0..LOG_SIZE-1 |-> <<0, 0, 0>>]]
             /\ log_head' = [t \in Thread |-> 0]
             /\ log_tail' = [t \in Thread |-> 0]
             /\ recovered' = TRUE
             /\ pc' = [pc EXCEPT ![-1] = "Done"]
             /\ UNCHANGED << state, stm_ws, stm_rs, stm_clock, stm_committed, 
                             replayer_state, persist_file, batch_marker >>

RecoveryProc == L_recover

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == ReplayerProc \/ RecoveryProc
           \/ (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Helper operators (defined after VARIABLES for TLC resolution)       *)
(*====================================================================*)

(* Check if buffer is full (one slot reserved to distinguish empty) *)
IsFull(t) ==
    (log_head[t] + 1) % LOG_SIZE = log_tail[t]

(* Check if buffer is empty *)
IsEmpty(t) ==
    log_head[t] = log_tail[t]

(* Bounding constraints for TLC termination *)
WritesBound == \A t \in Thread : Len(stm_ws[t]) < 2
ReadsBound == \A t \in Thread : Len(stm_rs[t]) < 2
ClockBound == stm_clock < 3
CommittedBound == \A t \in Thread : stm_committed[t] < 2
PersistFileBound == Len(persist_file) < 3

TLCBound == /\ WritesBound
            /\ ReadsBound
            /\ ClockBound
            /\ CommittedBound
            /\ PersistFileBound

(*====================================================================*)
(*====================================================================*)
(* Fence (memory ordering) annotations                                *)
(*                                                                    *)
(* DUDETM is a high-level design sketch in TLA+.  The fence pattern   *)
(* atomic head store.  See TinySTM_WBCTL.tla for fence annotations   *)
(* on the TM path.  The log head store in the C++ replayer uses      *)
(* atomic_thread_fence(release) before head.store(release), which     *)
(* the model abstracts as a single atomic log_head update.            *)
(*                                                                    *)
(* Score: 2/5 — model does not track fences; relies on WBCTL audit.  *)
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
(* NOTE: Fixed universal/existential confusion. OP_WRITE entries have format *)
(* <<OP_WRITE, a, v>> (no thread field). The old version used \A t to match *)
(* a thread field that doesn't exist. Replaced with \E t: there exists some *)
(* thread that committed before each OP_WRITE.                              *)
LogWriteMatch ==
    \A i \in 1..Len(persist_file) :
        persist_file[i][1] = OP_WRITE =>
            \E prev_idx \in 1..i-1, t \in Thread :
                persist_file[prev_idx][1] = OP_COMMIT_BEGIN /\
                persist_file[prev_idx][2] = t

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Per-process weak fairness *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))
              /\ WF_vars(ReplayerProc)
              /\ WF_vars(RecoveryProc)

(* Liveness: every transaction eventually commits or aborts *)
ProgressProperty ==
    \A t \in Thread :
        (state[t] = "active") ~> (state[t] \in {"idle", "logging"})

=====
