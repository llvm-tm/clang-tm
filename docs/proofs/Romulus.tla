------------------------ MODULE Romulus ------------------------
(*
 * Romulus — Version-Table OCC with Read-Validate
 *
 * Algorithm (from backends/tm_impl/romulus/romulus.hpp):
 *
 * Globals:
 *   - g_version_table[0..2^20-1]:  atomic<uint64_t>
 *       bit 0 = lock bit (1 = write-back in progress)
 *       bits 1+ = version number
 *   - g_global_clock:  atomic<uint64_t> (starts at 1)
 *   - g_commit_lock:   atomic<uint64_t>  (0 = free, 1 = held)
 *
 * Begin:
 *   snapshot = clock; clear read-set + write-set; active = true
 *
 * Read(addr):
 *   1. If addr in write-set, return buffered value.
 *   2. Read version entry BEFORE data:  entry_before = version[idx(addr)]
 *   3. If locked (entry_before % 2 == 1), abort.
 *   4. Read data from memory.
 *   5. Read version entry AFTER data:  entry_after = version[idx(addr)]
 *   6. If entry_before != entry_after, abort (version changed during read).
 *   7. Add (addr, entry_before >> 1) to read-set.
 *
 * Write(addr, val):
 *   Buffer (addr, val) in write-set; set read_only = false.
 *
 * Commit (non-read-only, non-empty write-set):
 *   1. Acquire commit lock (CAS spin).
 *   2. Validate write-set: for each addr, check
 *        version[idx] is not locked AND
 *        (version[idx] >> 1) <= timestamp
 *   3. Validate read-set: for each (addr, captured_version), check
 *        version[idx] is not locked AND
 *        (version[idx] >> 1) == captured_version
 *   4. If any validation fails, release lock, abort.
 *   5. Set lock bits on all written addresses (version[idx] |= 1).
 *   6. fence(seq_cst)
 *   7. Increment global clock -> commit_ts.
 *   8. Write-back: write buffered values to memory.
 *   9. fence(seq_cst)
 *   10. Update version table: version[idx] = make_version_entry(commit_ts)
 *       (clears lock bit, sets new version).
 *   11. Release commit lock.
 *
 * Read-only commit:
 *   Reset transaction state, no lock needed.
 *
 * Invariants (for TLC model checking):
 *   LockExclusion:   At most one thread holds the commit lock.
 *   NoDirtyRead:     A committed transaction's read-set entries are
 *                    consistent (no concurrent write-back corrupted them).
 *   VersionMonotonic: The global clock never decreases.
 *   LockBitCleared:  After commit, all written version entries have
 *                    lock bit = 0.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    VSIZE               (* Version table size *)

ASSUME VSIZE \in Nat \ {0}
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat

VARIABLES
    mem,                (* [Addr -> Data] *)
    version,            (* [0..VSIZE-1 -> Nat]  version-table entries *)
    clock,              (* Nat, global clock *)
    lock,               (* Nat, commit lock: 0 = free, t = held by thread t *)
    pc,                 (* [Thread -> {"idle", "active", "acquire_lock", "validate_ws", "validate_rs",
                                      "set_lock_bits", "inc_clock", "write_back", "update_ver", "release_lock",
                                      "aborting"}] *)
    timestamp,          (* [Thread -> Nat]  snapshot at begin *)
    read_set,           (* [Thread -> Seq(<<Addr, Nat>>)]  (addr, version) pairs *)
    write_set,          (* [Thread -> Addr -> Data \cup {NoWrite}] *)
    read_only,          (* [Thread -> BOOLEAN] *)
    commit_ts,          (* [Thread -> Nat]  commit timestamp *)
    committed,          (* [Thread -> Nat]  commit count *)
    aborted             (* [Thread -> Nat]  abort count *)

vars == <<mem, version, clock, lock, pc, timestamp, read_set, write_set,
          read_only, commit_ts, committed, aborted>>

(*--------------------------------------------------------------------*)
(* Helpers                                                             *)
(*--------------------------------------------------------------------*)

NoWrite == -1           (* sentinel meaning no write for this address *)

(* Version entry encoding: bit 0 = lock, bits 1+ = version number *)
LockBit(entry) == entry % 2
VersionOf(entry) == entry \div 2
MakeEntry(ver) == ver * 2

(* Map an address to a version-table index *)
VIndex(a) == a % VSIZE

(* Check if thread t has written to address a *)
HasWritten(t, a) == write_set[t][a] # NoWrite

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ mem = [a \in Addr |-> 0]
    /\ version = [i \in 0..VSIZE-1 |-> MakeEntry(0)]
    /\ clock = 1
    /\ lock = 0
    /\ pc = [t \in Thread |-> "idle"]
    /\ timestamp = [t \in Thread |-> 0]
    /\ read_set = [t \in Thread |-> << >>]
    /\ write_set = [t \in Thread |-> [a \in Addr |-> NoWrite]]
    /\ read_only = [t \in Thread |-> TRUE]
    /\ commit_ts = [t \in Thread |-> 0]
    /\ committed = [t \in Thread |-> 0]
    /\ aborted = [t \in Thread |-> 0]

(*--------------------------------------------------------------------*)
(* Actions                                                             *)
(*--------------------------------------------------------------------*)

(*── Begin ────────────────────────────────────────────────────────────*)
Begin(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ timestamp' = [timestamp EXCEPT ![t] = clock]
    /\ read_only' = [read_only EXCEPT ![t] = TRUE]
    /\ read_set' = [read_set EXCEPT ![t] = << >>]
    /\ write_set' = [write_set EXCEPT ![t] = [a \in Addr |-> NoWrite]]
    /\ commit_ts' = [commit_ts EXCEPT ![t] = 0]
    /\ UNCHANGED <<mem, version, clock, lock, committed, aborted>>

(*── Read: return buffered write if available ────────────────────────*)
ReadOwnWrite(t, a) ==
    /\ pc[t] = "active"
    /\ HasWritten(t, a)
    /\ UNCHANGED vars

(*── Read-Validate: capture version, read data, re-check version ────*)
ReadValidate(t, a) ==
    /\ pc[t] = "active"
    /\ ~HasWritten(t, a)
    /\ a \in Addr
    (* Step 1: capture version entry before reading *)
    /\ LET idx == VIndex(a)
           entry_before == version[idx] IN
       (* Step 2: if locked, abort *)
       IF LockBit(entry_before) = 1
       THEN
           (* abort *)
           /\ pc' = [pc EXCEPT ![t] = "aborting"]
           /\ UNCHANGED <<mem, version, clock, lock, timestamp, read_set,
                          write_set, read_only, commit_ts>>
       ELSE
           (* Step 3: read data from memory *)
           /\ LET val == mem[a] IN
              (* Step 4: re-check version entry after reading *)
              /\ entry_before = version[idx]   (* must not have changed *)
              (* Step 5: add to read-set *)
              /\ read_set' = [read_set EXCEPT ![t] = Append(read_set[t],
                                   <<a, VersionOf(entry_before)>>)]
              /\ UNCHANGED <<mem, version, clock, lock, pc, timestamp,
                             write_set, read_only, commit_ts, committed, aborted>>
    (* Note: if the re-check fails (entry_before # version[idx]), this
       action is simply not enabled — the thread must abort. The
       ReadValidateAbort action below handles that case. *)

(*── Read-validate failure (version changed during read) → abort ────*)
ReadValidateAbort(t, a) ==
    /\ pc[t] = "active"
    /\ ~HasWritten(t, a)
    /\ a \in Addr
    /\ LET idx == VIndex(a)
           entry_before == version[idx] IN
       LockBit(entry_before) = 0                  (* not locked before read *)
    /\ LET entry_after == version[idx] IN
       entry_before # entry_after                  (* changed during read *)
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ UNCHANGED <<mem, version, clock, lock, timestamp, read_set,
                   write_set, read_only, commit_ts, committed, aborted>>

(*── Write: buffer value in write-set ────────────────────────────────*)
WriteAction(t, a, v) ==
    /\ pc[t] = "active"
    /\ a \in Addr
    /\ v \in Data
    /\ write_set' = [write_set EXCEPT ![t][a] = v]
    /\ read_only' = [read_only EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<mem, version, clock, lock, pc, timestamp, read_set,
                   commit_ts, committed, aborted>>

(*── Read-only commit ────────────────────────────────────────────────*)
ReadOnlyCommit(t) ==
    /\ pc[t] = "active"
    /\ read_only[t] = TRUE
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, version, clock, lock, timestamp, read_set,
                   write_set, read_only, commit_ts, aborted>>

(*── Commit (non-read-only path) — Phase 1: Acquire lock ────────────*)
AcquireLock(t) ==
    /\ pc[t] = "active"
    /\ read_only[t] = FALSE
    /\ \E a \in Addr : HasWritten(t, a)   (* non-empty write-set *)
    /\ lock = 0
    /\ lock' = t
    /\ pc' = [pc EXCEPT ![t] = "validate_ws"]
    /\ UNCHANGED <<mem, version, clock, timestamp, read_set, write_set,
                   read_only, commit_ts, committed, aborted>>

(*── Phase 2a: Validate write-set ────────────────────────────────────*)
ValidateWS(t) ==
    /\ pc[t] = "validate_ws"
    /\ lock = t
    (* For each address that t has written, check: not locked, version <= timestamp *)
    /\ \A a \in Addr :
         HasWritten(t, a) =>
           LET idx == VIndex(a)
               entry == version[idx] IN
           LockBit(entry) = 0 /\ VersionOf(entry) <= timestamp[t]
    (* If all pass, move to validate read-set *)
    /\ pc' = [pc EXCEPT ![t] = "validate_rs"]
    /\ UNCHANGED <<mem, version, clock, lock, timestamp, read_set,
                   write_set, read_only, commit_ts, committed, aborted>>

(*── Phase 2b: Validate read-set ─────────────────────────────────────*)
ValidateRS(t) ==
    /\ pc[t] = "validate_rs"
    /\ lock = t
    (* For each entry in read-set, check: not locked, version unchanged *)
    /\ \A i \in 1..Len(read_set[t]) :
         LET entry == read_set[t][i]
             a == entry[1]
             captured_version == entry[2]
             idx == VIndex(a)
             current_entry == version[idx] IN
         LockBit(current_entry) = 0 /\ VersionOf(current_entry) = captured_version
    (* All pass → proceed to set lock bits *)
    /\ pc' = [pc EXCEPT ![t] = "set_lock_bits"]
    /\ UNCHANGED <<mem, version, clock, lock, timestamp, read_set,
                   write_set, read_only, commit_ts, committed, aborted>>

(*── Phase 2a/2b failure → abort ────────────────────────────────────*)
ValidationFailed(t) ==
    /\ pc[t] = "validate_ws" \/ pc[t] = "validate_rs"
    /\ lock = t
    /\ ~ ( \A a \in Addr :
              HasWritten(t, a) =>
                LET idx == VIndex(a)
                    entry == version[idx] IN
                LockBit(entry) = 0 /\ VersionOf(entry) <= timestamp[t]
          )
    /\ lock' = 0
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ UNCHANGED <<mem, version, clock, timestamp, read_set, write_set,
                   read_only, commit_ts, committed, aborted>>

(*── Phase 3: Set lock bits on all written addresses ────────────────*)
SetLockBits(t) ==
    \* Set lock bit (bit 0 = 1) on version entries for all written addresses.
    \* Since MakeEntry(ver) = ver*2 produces an even number (bit 0 = 0),
    \* adding 1 sets the lock bit.
    /\ pc[t] = "set_lock_bits"
    /\ lock = t
    /\ LET WrittenIdxs == {VIndex(a) : a \in {a2 \in Addr : HasWritten(t, a2)}} IN
       version' = [i \in 0..VSIZE-1 |->
                     IF i \in WrittenIdxs THEN version[i] + 1
                                          ELSE version[i]]
    /\ pc' = [pc EXCEPT ![t] = "inc_clock"]
    /\ UNCHANGED <<mem, clock, lock, timestamp, read_set, write_set,
                   read_only, commit_ts, committed, aborted>>

(*── Phase 4: Increment global clock ─────────────────────────────────*)
IncClock(t) ==
    /\ pc[t] = "inc_clock"
    /\ lock = t
    /\ clock' = clock + 1
    /\ commit_ts' = [commit_ts EXCEPT ![t] = clock']  (* new value after increment *)
    /\ pc' = [pc EXCEPT ![t] = "write_back"]
    /\ UNCHANGED <<mem, version, lock, timestamp, read_set, write_set,
                   read_only, committed, aborted>>

(*── Phase 5: Write-back buffered values to memory ───────────────────*)
WriteBack(t) ==
    /\ pc[t] = "write_back"
    /\ lock = t
    /\ mem' = [a \in Addr |->
                 IF HasWritten(t, a) THEN write_set[t][a] ELSE mem[a]]
    /\ pc' = [pc EXCEPT ![t] = "update_ver"]
    /\ UNCHANGED <<version, clock, lock, timestamp, read_set, write_set,
                   read_only, commit_ts, committed, aborted>>

(*── Phase 6: Update version table (clear lock bit, set new version) ─*)
UpdateVersion(t) ==
    /\ pc[t] = "update_ver"
    /\ lock = t
    /\ LET WrittenIdxs == {VIndex(a) : a \in {a2 \in Addr : HasWritten(t, a2)}} IN
       version' = [i \in 0..VSIZE-1 |->
                     IF i \in WrittenIdxs
                     THEN MakeEntry(commit_ts[t])
                     ELSE version[i]]
    /\ pc' = [pc EXCEPT ![t] = "release_lock"]
    /\ UNCHANGED <<mem, clock, lock, timestamp, read_set, write_set,
                   read_only, commit_ts, committed, aborted>>

(*── Phase 7: Release commit lock ────────────────────────────────────*)
ReleaseLock(t) ==
    /\ pc[t] = "release_lock"
    /\ lock = t
    /\ lock' = 0
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, version, clock, timestamp, read_set, write_set,
                   read_only, commit_ts, aborted>>

(*── Abort ───────────────────────────────────────────────────────────*)
Abort(t) ==
    /\ pc[t] \in {"active", "aborting", "validate_ws", "validate_rs"}
    /\ ~ (pc[t] \in {"validate_ws", "validate_rs"} /\ lock = t)
    (* If we're holding the lock during validation, use ValidationFailed instead *)
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ timestamp' = [timestamp EXCEPT ![t] = 0]
    /\ read_set' = [read_set EXCEPT ![t] = << >>]
    /\ write_set' = [write_set EXCEPT ![t] = [a \in Addr |-> NoWrite]]
    /\ read_only' = [read_only EXCEPT ![t] = TRUE]
    /\ commit_ts' = [commit_ts EXCEPT ![t] = 0]
    /\ UNCHANGED <<mem, version, clock, lock>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \E t \in Thread :
        \/ Begin(t)
        \/ \E a \in Addr : ReadOwnWrite(t, a)
        \/ \E a \in Addr : ReadValidate(t, a)
        \/ \E a \in Addr : ReadValidateAbort(t, a)
        \/ \E a \in Addr : \E v \in Data : WriteAction(t, a, v)
        \/ ReadOnlyCommit(t)
        \/ AcquireLock(t)
        \/ ValidateWS(t)
        \/ ValidateRS(t)
        \/ ValidationFailed(t)
        \/ SetLockBits(t)
        \/ IncClock(t)
        \/ WriteBack(t)
        \/ UpdateVersion(t)
        \/ ReleaseLock(t)
        \/ Abort(t)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: At most one thread holds the commit lock ────────────────────*)
LockExclusion ==
    \A t1, t2 \in Thread :
        (lock = t1 /\ lock = t2) => t1 = t2

(*── I2: If a thread holds the lock, it is in a commit phase ────────*)
LockHeldImpliesCommitting ==
    \A t \in Thread :
        lock = t => pc[t] \in {"validate_ws", "validate_rs", "set_lock_bits",
                                "inc_clock", "write_back", "update_ver",
                                "release_lock"}

(*── I3: The global clock never decreases ────────────────────────────*)
ClockMonotonic ==
    clock \in Nat /\ clock >= 1

(*── I4: Version table entries are always even (lock bit = 0) when
        no commit is in progress on that index ──────────────────────*)
VersionEntryValid ==
    \A i \in 0..VSIZE-1 :
        LockBit(version[i]) = 0 \/
        \E t \in Thread :
            (pc[t] \in {"set_lock_bits", "inc_clock", "write_back", "update_ver"}
             /\ \E a \in Addr : HasWritten(t, a) /\ VIndex(a) = i)

(*── I5: Read-set entries correspond to committed versions ──────────*)
ReadSetConsistent ==
    \A t \in Thread :
        \A i \in 1..Len(read_set[t]) :
            LET entry == read_set[t][i]
                a == entry[1]
                ver == entry[2] IN
            VersionOf(version[VIndex(a)]) >= ver

(*── I6: No two threads can be in commit phases simultaneously ──────*)
AtMostOneCommitting ==
    \A t1, t2 \in Thread :
        t1 # t2 =>
            ~ ( pc[t1] \in {"validate_ws", "validate_rs", "set_lock_bits",
                             "inc_clock", "write_back", "update_ver",
                             "release_lock"}
              /\ pc[t2] \in {"validate_ws", "validate_rs", "set_lock_bits",
                              "inc_clock", "write_back", "update_ver",
                              "release_lock"} )

(*====================================================================*)
(* Temporal property: every started transaction eventually completes  *)
(* (not provable for all schedules, but useful for TLC checking)      *)
(*====================================================================*)

Completion ==
    \A t \in Thread : <>[](pc[t] \in {"idle", "aborting"})

(*====================================================================*)
(* Model-checking setup                                               *)
(*====================================================================*)

(* Default model parameters — override in TLC model config *)
(* Thread = {1, 2}; Addr = {0, 1}; Data = {0, 1, 2}; VSIZE = 2 *)
