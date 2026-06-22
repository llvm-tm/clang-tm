----------------------- MODULE LEFTRIGHT -----------------------
(*
 * LEFTRIGHT — Global-Clock OCC with Value-Based Validation
 *
 * Algorithm (from backends/tm_impl/leftright/leftright.hpp):
 *
 * Despite the name, this is NOT Left-Right Synchronization.
 * It is a global-clock OCC with value-based validation:
 *
 *   - g_clock: global version clock (monotonic counter)
 *   - g_commit_lock: spinlock serializing the commit path
 *   - Read-set: (addr, observed_version, captured_value)
 *   - Write-set: (addr, new_value)
 *
 *   Begin: snapshot = clock; active = true
 *   Read:  check write-set first (own writes visible).
 *          capture clock, read data, record (addr, clock, value).
 *   Write: buffer in write-set.
 *   Commit (non-read-only):
 *     1. Acquire commit lock (CAS spin).
 *     2. Validate phase 1 (optimistic): check read-set versions
 *        haven't changed beyond snapshot (get_clock() <= observed_version).
 *     3. Phase 2 (under lock): for each read-set entry, re-read
 *        data from memory and compare with captured_value (memcmp).
 *        This detects actual conflicts without false aborts from
 *        concurrent non-conflicting commits.
 *     4. If valid: increment clock, write-back, release lock.
 *     5. If invalid: release lock, abort.
 *
 *   Read-only commit: reset state, no lock needed.
 *
 * Invariants:
 *   LockExclusion:     At most one thread holds the commit lock.
 *   NoDirtyRead:       A committed TX's read-set values are consistent.
 *   ValueValidation:   After commit, all read-set entries match
 *                      their captured values (no concurrent modification).
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data                (* Set of possible data values *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat

VARIABLES
    mem,                (* [Addr -> Data] *)
    clock,              (* Nat: global version clock *)
    commit_lock,        (* 0 = free, t = held by t *)
    pc,                 (* [Thread -> {"idle", "active", "acquire_lock",
                                      "validate_p1", "validate_p2",
                                      "inc_clock", "write_back",
                                      "release_lock", "aborting"}] *)
    snapshot,           (* [Thread -> Nat]: clock snapshot at begin *)
    read_set,           (* [Thread -> Seq(<<Addr, Nat, Data>>)]
                           (addr, observed_version, captured_value) *)
    write_set,          (* [Thread -> Addr -> Data \cup {NoWrite}] *)
    read_only,          (* [Thread -> BOOLEAN] *)
    commit_count,       (* [Thread -> Nat] *)
    abort_count         (* [Thread -> Nat] *)

vars == <<mem, clock, commit_lock, pc, snapshot, read_set,
          write_set, read_only, commit_count, abort_count>>

NoWrite == 0 - 1
HasWritten(t, a) == write_set[t][a] # NoWrite

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ mem = [a \in Addr |-> 0]
    /\ clock = 1
    /\ commit_lock = 0
    /\ pc = [t \in Thread |-> "idle"]
    /\ snapshot = [t \in Thread |-> 0]
    /\ read_set = [t \in Thread |-> << >>]
    /\ write_set = [t \in Thread |-> [a \in Addr |-> NoWrite]]
    /\ read_only = [t \in Thread |-> TRUE]
    /\ commit_count = [t \in Thread |-> 0]
    /\ abort_count = [t \in Thread |-> 0]

(*--------------------------------------------------------------------*)
(* Actions                                                             *)
(*--------------------------------------------------------------------*)

(*── Begin ──────────────────────────────────────────────────────────*)
Begin(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ snapshot' = [snapshot EXCEPT ![t] = clock]
    /\ read_only' = [read_only EXCEPT ![t] = TRUE]
    /\ read_set' = [read_set EXCEPT ![t] = << >>]
    /\ write_set' = [write_set EXCEPT ![t] = [a \in Addr |-> NoWrite]]
    /\ UNCHANGED <<mem, clock, commit_lock, commit_count, abort_count>>

(*── Read (check write-set first, then read with value capture) ────*)
Read(t, a) ==
    /\ pc[t] = "active"
    /\ a \in Addr
    /\ IF HasWritten(t, a)
       THEN
           (* Return own buffered write — no read-set entry needed *)
           /\ UNCHANGED vars
       ELSE
           (* Capture: (addr, clock, value) *)
           /\ LET ver == clock
                  val == mem[a] IN
              read_set' = [read_set EXCEPT ![t] =
                             Append(read_set[t], <<a, ver, val>>)]
              /\ UNCHANGED <<mem, clock, commit_lock, pc, snapshot,
                              write_set, read_only, commit_count,
                              abort_count>>

(*── Write: buffer in write-set ─────────────────────────────────────*)
Write(t, a, v) ==
    /\ pc[t] = "active"
    /\ a \in Addr
    /\ v \in Data
    /\ write_set' = [write_set EXCEPT ![t][a] = v]
    /\ read_only' = [read_only EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<mem, clock, commit_lock, pc, snapshot, read_set,
                   commit_count, abort_count>>

(*── Read-only commit ──────────────────────────────────────────────*)
ReadOnlyCommit(t) ==
    /\ pc[t] = "active"
    /\ read_only[t] = TRUE
    /\ commit_count' = [commit_count EXCEPT ![t] = commit_count[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, clock, commit_lock, snapshot, read_set,
                   write_set, read_only, abort_count>>

(*── Acquire commit lock ───────────────────────────────────────────*)
AcquireLock(t) ==
    /\ pc[t] = "active"
    /\ read_only[t] = FALSE
    /\ \E a \in Addr : HasWritten(t, a)
    /\ commit_lock = 0
    /\ commit_lock' = t
    /\ pc' = [pc EXCEPT ![t] = "validate_p1"]
    /\ UNCHANGED <<mem, clock, snapshot, read_set, write_set,
                   read_only, commit_count, abort_count>>

(*── Phase 1 (optimistic): check read-set versions haven't advanced ─*)
ValidateP1(t) ==
    /\ pc[t] = "validate_p1"
    /\ commit_lock = t
    (* Fast check: global clock didn't advance beyond any observed_version *)
    /\ \A i \in 1..Len(read_set[t]) :
         LET entry == read_set[t][i]
             observed_ver == entry[2] IN
         observed_ver <= snapshot[t]
    (* If all read-set entries have observed_ver <= snapshot, proceed *)
    /\ pc' = [pc EXCEPT ![t] = "validate_p2"]
    /\ UNCHANGED <<mem, clock, commit_lock, snapshot, read_set,
                   write_set, read_only, commit_count, abort_count>>

(*── Phase 2 (under lock): value-based validation (memcmp) ─────────*)
ValidateP2(t) ==
    /\ pc[t] = "validate_p2"
    /\ commit_lock = t
    (* Re-read each read-set address from memory and compare with
       captured_value.  This detects actual data conflicts. *)
    /\ \A i \in 1..Len(read_set[t]) :
         LET entry == read_set[t][i]
             a == entry[1]
             captured_val == entry[3] IN
         mem[a] = captured_val
    (* All values match → proceed to commit *)
    /\ pc' = [pc EXCEPT ![t] = "inc_clock"]
    /\ UNCHANGED <<mem, clock, commit_lock, snapshot, read_set,
                   write_set, read_only, commit_count, abort_count>>

(*── Phase 1 or 2 failure → abort ──────────────────────────────────*)
ValidationFailed(t) ==
    /\ pc[t] \in {"validate_p1", "validate_p2"}
    /\ commit_lock = t
    /\ IF pc[t] = "validate_p1"
       THEN ~ (\A i \in 1..Len(read_set[t]) :
                 LET entry == read_set[t][i]
                     observed_ver == entry[2] IN
                 observed_ver <= snapshot[t])
       ELSE ~ (\A i \in 1..Len(read_set[t]) :
                 LET entry == read_set[t][i]
                     a == entry[1]
                     captured_val == entry[3] IN
                 mem[a] = captured_val)
    /\ commit_lock' = 0
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ UNCHANGED <<mem, clock, snapshot, read_set, write_set,
                   read_only, commit_count, abort_count>>

(*── Increment clock ───────────────────────────────────────────────*)
IncClock(t) ==
    /\ pc[t] = "inc_clock"
    /\ commit_lock = t
    /\ clock' = clock + 1
    /\ pc' = [pc EXCEPT ![t] = "write_back"]
    /\ UNCHANGED <<mem, commit_lock, snapshot, read_set, write_set,
                   read_only, commit_count, abort_count>>

(*── Write-back ────────────────────────────────────────────────────*)
WriteBack(t) ==
    /\ pc[t] = "write_back"
    /\ commit_lock = t
    /\ mem' = [a \in Addr |->
                 IF HasWritten(t, a) THEN write_set[t][a] ELSE mem[a]]
    /\ pc' = [pc EXCEPT ![t] = "release_lock"]
    /\ UNCHANGED <<clock, commit_lock, snapshot, read_set, write_set,
                   read_only, commit_count, abort_count>>

(*── Release lock ──────────────────────────────────────────────────*)
ReleaseLock(t) ==
    /\ pc[t] = "release_lock"
    /\ commit_lock = t
    /\ commit_lock' = 0
    /\ commit_count' = [commit_count EXCEPT ![t] = commit_count[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, clock, snapshot, read_set, write_set,
                   read_only, abort_count>>

(*── Abort (without holding lock) ──────────────────────────────────*)
Abort(t) ==
    /\ pc[t] \in {"active", "aborting"}
    /\ commit_lock # t
    /\ abort_count' = [abort_count EXCEPT ![t] = abort_count[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ read_set' = [read_set EXCEPT ![t] = << >>]
    /\ write_set' = [write_set EXCEPT ![t] = [a \in Addr |-> NoWrite]]
    /\ read_only' = [read_only EXCEPT ![t] = TRUE]
    /\ UNCHANGED <<mem, clock, commit_lock, snapshot, commit_count>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \E t \in Thread :
        \/ Begin(t)
        \/ \E a \in Addr : Read(t, a)
        \/ \E a \in Addr : \E v \in Data : Write(t, a, v)
        \/ ReadOnlyCommit(t)
        \/ AcquireLock(t)
        \/ ValidateP1(t)
        \/ ValidateP2(t)
        \/ ValidationFailed(t)
        \/ IncClock(t)
        \/ WriteBack(t)
        \/ ReleaseLock(t)
        \/ Abort(t)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: At most one thread holds the commit lock ──────────────────*)
LockExclusion ==
    \A t1, t2 \in Thread :
        (commit_lock = t1 /\ commit_lock = t2) => t1 = t2

(*── I2: Lock holder is in a valid commit phase ────────────────────*)
LockHolderCommitting ==
    \A t \in Thread :
        commit_lock = t =>
            pc[t] \in {"validate_p1", "validate_p2", "inc_clock",
                        "write_back", "release_lock"}

(*── I3: Read-set entries have consistent versions ─────────────────*)
ReadSetConsistent ==
    \A t \in Thread :
        pc[t] = "idle" =>
            \A i \in 1..Len(read_set[t]) :
                LET entry == read_set[t][i]
                    a == entry[1]
                    ver == entry[2] IN
                ver <= clock   (* version is not from the future *)

(*── I4: No dirty reads — after commit, all read values are current ─*)
NoDirtyRead ==
    \A t \in Thread :
        pc[t] = "idle" =>
            \A i \in 1..Len(read_set[t]) :
                LET entry == read_set[t][i]
                    a == entry[1]
                    ver == entry[2]
                    val == entry[3] IN
                mem[a] = val

(*── I5: At most one thread in commit phases ───────────────────────*)
AtMostOneCommitting ==
    \A t1, t2 \in Thread :
        t1 # t2 =>
            ~ ( pc[t1] \in {"validate_p1", "validate_p2", "inc_clock",
                             "write_back", "release_lock"}
              /\ pc[t2] \in {"validate_p1", "validate_p2", "inc_clock",
                              "write_back", "release_lock"} )

====
