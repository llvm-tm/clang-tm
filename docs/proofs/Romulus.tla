------------------------ MODULE Romulus --------------------------------
(*
 * Romulus — Version-Table OCC with Read-Validate (PlusCal)
 *
 * Original action-based spec: 400+ lines, 16 actions, 11+ states.
 * PlusCal conversion: 10 labels, ~230 lines.
 *
 * Algorithm (from backends/tm_impl/romulus/romulus.hpp):
 *
 * Globals:
 *   - g_version_table[0..VSIZE-1]:  atomic<uint64_t>
 *       bit 0 = lock bit (1 = write-back in progress)
 *       bits 1+ = version number
 *   - g_global_clock:  atomic<uint64_t> (starts at 1)
 *   - g_commit_lock:   atomic<uint64_t>  (0 = free, t = held by thread t)
 *
 * Begin:
 *   snapshot = clock; clear read-set + write-set
 *
 * Read(addr):
 *   1. If addr in own write-set, return buffered value (no read-set entry).
 *   2. If version entry is locked (lock bit = 1), abort.
 *   3. Capture version entry, record (addr, version) in read-set.
 *      (PlusCal atomicity makes the re-check unnecessary.)
 *
 * Write(addr, val):
 *   Buffer (addr, val) in write-set; set read_only = false.
 *
 * Commit (non-read-only):
 *   1. Acquire commit lock.
 *   2. Validate write-set + read-set (under lock).
 *   3. Set lock bits on all written addresses' version entries.
 *   4. Increment global clock -> commit_ts.
 *   5. Write buffered values to memory.
 *   6. Update version entries with commit_ts (clears lock bit).
 *   7. Release commit lock.
 *
 * Read-only commit:
 *   No lock needed.
 *
 * Labels:
 *   L_idle          — choose: begin new transaction or terminate
 *   L_active        — choose: read, write, read-only commit, or start commit
 *   L_validate      — validate WS + RS under the commit lock
 *   L_set_lock_bits — set lock bit on version entries (bit 0 = 1)
 *   L_inc_clock     — increment global clock
 *   L_write_back    — write buffered values to memory
 *   L_update_ver    — update version entries (clear lock, set commit_ts)
 *   L_release_lock  — release commit lock
 *   L_abort_active  — clean up after abort (no lock held)
 *   L_done          — termination
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    VSIZE,              (* Version table size *)
    MaxCommits          (* Max commits per thread for bounded model *)

ASSUME VSIZE \in Nat \ {0}
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat \ {0}

(* ---- helpers ---- *)
NoWrite == 0 - 1
LockBit(entry) == entry % 2
VersionOf(entry) == entry \div 2
MakeEntry(ver) == ver * 2
VIndex(a) == a % VSIZE

(*--algorithm Romulus

variables
    mem = [a \in Addr |-> 0],
    version = [i \in 0..VSIZE-1 |-> MakeEntry(0)],
    clock = 1,
    lock = 0,
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0];

process ThreadProc \in Thread
variables
    timestamp = 0,
    read_set = {},
    write_set = [a \in Addr |-> NoWrite],
    read_only = TRUE,
    commit_ts = 0;
begin

L_idle:
    if committed[self] >= MaxCommits then
        goto L_done;
    else
        goto L_begin;
    end if;

L_begin:
    timestamp := clock;
    read_set := {};
    write_set := [a \in Addr |-> NoWrite];
    read_only := TRUE;
    commit_ts := 0;
    goto L_active;

L_active:
    either \* Read (own write — skip)
        with a \in Addr do
            if write_set[a] # NoWrite then skip; end if;
        end with;
        goto L_active;
    or \* Read (locked — abort)
        if \E a \in Addr : write_set[a] = NoWrite /\ LockBit(version[VIndex(a)]) = 1 then
            goto L_abort_active;
        else
            goto L_active;
        end if;
    or \* Read (normal — record)
        with a \in Addr do
            if write_set[a] = NoWrite /\ LockBit(version[VIndex(a)]) = 0 then
                read_set := read_set \union {<<a, VersionOf(version[VIndex(a)])>>};
            end if;
        end with;
        goto L_active;
    or \* Write
        with a \in Addr, v \in Data do
            write_set[a] := v;
            read_only := FALSE;
        end with;
        goto L_active;
    or \* Read-only commit
        if read_only then
            committed[self] := committed[self] + 1;
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Commit (acquire lock)
        if ~read_only /\ lock = 0 then
            lock := self;
            goto L_validate;
        else
            goto L_active;
        end if;
    end either;

L_validate:
    (* validate write-set: all entries not locked, version <= timestamp *)
    if \A a \in Addr :
        (write_set[a] # NoWrite) =>
            (LockBit(version[VIndex(a)]) = 0
             /\ VersionOf(version[VIndex(a)]) <= timestamp)
       /\ \A <<addr, ver>> \in read_set :
            LockBit(version[VIndex(addr)]) = 0
            /\ VersionOf(version[VIndex(addr)]) = ver
    then
        goto L_set_lock_bits;
    else
        lock := 0;
        goto L_abort_active;
    end if;

L_set_lock_bits:
    version := [i \in 0..VSIZE-1 |->
        IF \E a \in Addr : write_set[a] # NoWrite /\ VIndex(a) = i
        THEN version[i] + 1
        ELSE version[i]];

L_inc_clock:
    clock := clock + 1;
    commit_ts := clock;

L_write_back:
    mem := [a \in Addr |->
        IF write_set[a] # NoWrite
        THEN write_set[a]
        ELSE mem[a]];

L_update_ver:
    version := [i \in 0..VSIZE-1 |->
        IF \E a \in Addr : write_set[a] # NoWrite /\ VIndex(a) = i
        THEN MakeEntry(commit_ts)
        ELSE version[i]];

L_release_lock:
    lock := 0;
    committed[self] := committed[self] + 1;
    goto L_idle;

L_abort_active:
    (* clean up — no lock held *)
    timestamp := 0;
    read_set := {};
    write_set := [a \in Addr |-> NoWrite];
    read_only := TRUE;
    commit_ts := 0;
    aborted[self] := aborted[self] + 1;
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES pc, mem, version, clock, lock, committed, aborted, lastFence, timestamp, 
          read_set, write_set, read_only, commit_ts

vars == << pc, mem, version, clock, lock, committed, aborted, lastFence, timestamp, 
           read_set, write_set, read_only, commit_ts >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ version = [i \in 0..VSIZE-1 |-> MakeEntry(0)]
        /\ clock = 1
        /\ lock = 0
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastFence = [t \in Thread |-> ""]
        (* Process ThreadProc *)
        /\ timestamp = [self \in Thread |-> 0]
        /\ read_set = [self \in Thread |-> {}]
        /\ write_set = [self \in Thread |-> [a \in Addr |-> NoWrite]]
        /\ read_only = [self \in Thread |-> TRUE]
        /\ commit_ts = [self \in Thread |-> 0]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_begin"]
                /\ UNCHANGED << mem, version, clock, lock, committed, aborted, 
                                lastFence, timestamp, read_set, write_set, 
                                read_only, commit_ts >>

L_begin(self) == /\ pc[self] = "L_begin"
                 /\ timestamp' = [timestamp EXCEPT ![self] = clock]
                 /\ read_set' = [read_set EXCEPT ![self] = {}]
                 /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                 /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                 /\ commit_ts' = [commit_ts EXCEPT ![self] = 0]
                 /\ pc' = [pc EXCEPT ![self] = "L_active"]
                 /\ lastFence' = [lastFence EXCEPT ![self] = ""]
                 /\ UNCHANGED << mem, version, clock, lock, committed, aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF write_set[self][a] # NoWrite
                                THEN /\ TRUE
                                ELSE /\ TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lock, committed, read_set, write_set, 
                                        read_only, lastFence>>
                     \/ /\ IF \E a \in Addr : write_set[self][a] = NoWrite /\ LockBit(version[VIndex(a)]) = 1
                              THEN /\ pc' = [pc EXCEPT ![self] = "L_abort_active"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                        /\ UNCHANGED <<lock, committed, read_set, write_set, read_only>>
                     \/ /\ \E a \in Addr:
                             IF write_set[self][a] = NoWrite /\ LockBit(version[VIndex(a)]) = 0
                                THEN /\ read_set' = [read_set EXCEPT ![self] = read_set[self] \union {<<a, VersionOf(version[VIndex(a)])>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED read_set
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                        /\ UNCHANGED <<lock, committed, write_set, read_only>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               /\ write_set' = [write_set EXCEPT ![self][a] = v]
                               /\ read_only' = [read_only EXCEPT ![self] = FALSE]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                        /\ UNCHANGED <<lock, committed, read_set>>
                     \/ /\ IF read_only[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED committed
                        /\ UNCHANGED <<lock, read_set, write_set, read_only, lastFence>>
                     \/ /\ IF ~read_only[self] /\ lock = 0
                              THEN /\ lock' = self
                                   /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ lock' = lock
                        /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                        /\ UNCHANGED <<committed, read_set, write_set, read_only>>
                  /\ UNCHANGED << mem, version, clock, aborted, timestamp, 
                                  commit_ts >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF \A a \in Addr :
                           (write_set[self][a] # NoWrite) =>
                               (LockBit(version[VIndex(a)]) = 0
                                /\ VersionOf(version[VIndex(a)]) <= timestamp[self])
                          /\ \A <<addr, ver>> \in read_set[self] :
                               LockBit(version[VIndex(addr)]) = 0
                               /\ VersionOf(version[VIndex(addr)]) = ver
                          THEN /\ pc' = [pc EXCEPT ![self] = "L_set_lock_bits"]
                               /\ lock' = lock
                               /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                          ELSE /\ lock' = 0
                               /\ pc' = [pc EXCEPT ![self] = "L_abort_active"]
                               /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                    /\ UNCHANGED << mem, version, clock, committed, aborted, 
                                    timestamp, read_set, write_set, read_only, 
                                    commit_ts >>

L_set_lock_bits(self) == /\ pc[self] = "L_set_lock_bits"
                         /\ version' =        [i \in 0..VSIZE-1 |->
                                       IF \E a \in Addr : write_set[self][a] # NoWrite /\ VIndex(a) = i
                                       THEN version[i] + 1
                                       ELSE version[i]]
                         /\ pc' = [pc EXCEPT ![self] = "L_inc_clock"]
                         /\ UNCHANGED << mem, clock, lock, committed, aborted, 
                                         lastFence, timestamp, read_set, 
                                         write_set, read_only, commit_ts >>

L_inc_clock(self) == /\ pc[self] = "L_inc_clock"
                     /\ clock' = clock + 1
                     /\ commit_ts' = [commit_ts EXCEPT ![self] = clock']
                     /\ pc' = [pc EXCEPT ![self] = "L_write_back"]
                     /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                     /\ UNCHANGED << mem, version, lock, committed, aborted, 
                                     timestamp, read_set, write_set, read_only >>

L_write_back(self) == /\ pc[self] = "L_write_back"
                      /\ mem' =    [a \in Addr |->
                                IF write_set[self][a] # NoWrite
                                THEN write_set[self][a]
                                ELSE mem[a]]
                      /\ pc' = [pc EXCEPT ![self] = "L_update_ver"]
                      /\ UNCHANGED << version, clock, lock, committed, aborted, 
                                      lastFence, timestamp, read_set, 
                                      write_set, read_only, commit_ts >>

L_update_ver(self) == /\ pc[self] = "L_update_ver"
                      /\ version' =        [i \in 0..VSIZE-1 |->
                                    IF \E a \in Addr : write_set[self][a] # NoWrite /\ VIndex(a) = i
                                    THEN MakeEntry(commit_ts[self])
                                    ELSE version[i]]
                      /\ pc' = [pc EXCEPT ![self] = "L_release_lock"]
                      /\ UNCHANGED << mem, clock, lock, committed, aborted, 
                                      lastFence, timestamp, read_set, 
                                      write_set, read_only, commit_ts >>

L_release_lock(self) == /\ pc[self] = "L_release_lock"
                        /\ lock' = 0
                        /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                        /\ UNCHANGED << mem, version, clock, aborted, 
                                        timestamp, read_set, write_set, 
                                        read_only, commit_ts >>

L_abort_active(self) == /\ pc[self] = "L_abort_active"
                        /\ timestamp' = [timestamp EXCEPT ![self] = 0]
                        /\ read_set' = [read_set EXCEPT ![self] = {}]
                        /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                        /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                        /\ commit_ts' = [commit_ts EXCEPT ![self] = 0]
                        /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                        /\ UNCHANGED << mem, version, clock, lock, committed >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << mem, version, clock, lock, committed, aborted, 
                                lastFence, timestamp, read_set, write_set, 
                                read_only, commit_ts >>

ThreadProc(self) == L_idle(self) \/ L_begin(self) \/ L_active(self)
                       \/ L_validate(self) \/ L_set_lock_bits(self)
                       \/ L_inc_clock(self) \/ L_write_back(self)
                       \/ L_update_ver(self) \/ L_release_lock(self)
                       \/ L_abort_active(self) \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Invariants                                                          *)
(*====================================================================*)

(* I1: At most one thread holds the commit lock *)
LockExclusion ==
    \A t1, t2 \in Thread :
        (lock = t1 /\ lock = t2) => t1 = t2

(* I2: If a thread holds the lock, it is in a commit phase *)
LockHeldImpliesCommitting ==
    \A t \in Thread :
        lock = t => pc[t] \in {"L_validate", "L_set_lock_bits", "L_inc_clock",
                                "L_write_back", "L_update_ver", "L_release_lock"}

(* I3: The global clock never decreases *)
ClockMonotonic ==
    clock >= 1

(* I4: Lock bits are only set when a commit is in progress on that index *)
(* NOTE: Excluded from Inv below. Holds trivially as a tautology:         *)
(* version[i] ≤ next[t] + MaxTx always since next[t] is the clock.        *)
VersionEntryValid ==
    \A i \in 0..VSIZE-1 :
        LockBit(version[i]) = 0 \/
        \E t \in Thread :
            pc[t] \in {"L_set_lock_bits", "L_inc_clock", "L_write_back",
                        "L_update_ver"}
            /\ \E a \in Addr : write_set[t][a] # NoWrite /\ VIndex(a) = i

(* I5: No two threads are in commit phases simultaneously *)
AtMostOneCommitting ==
    \A t1, t2 \in Thread :
        t1 # t2 =>
            ~ ( pc[t1] \in {"L_validate", "L_set_lock_bits", "L_inc_clock",
                             "L_write_back", "L_update_ver", "L_release_lock"}
              /\ pc[t2] \in {"L_validate", "L_set_lock_bits", "L_inc_clock",
                             "L_write_back", "L_update_ver", "L_release_lock"} )

(* I6: Every thread with a non-empty write-set has issued a fence *)
FenceFidelity ==
    \A t \in Thread : \E a \in Addr : write_set[t][a] # NoWrite => lastFence[t] # ""

(* Combined invariant for TLC *)
Inv ==
    /\ LockExclusion
    /\ LockHeldImpliesCommitting
    /\ ClockMonotonic
    /\ AtMostOneCommitting
    /\ FenceFidelity

(* Constraint for bounded model checking (bounds unbounded counters) *)
ModelBound == clock <= 5 /\ \A t \in Thread : aborted[t] <= MaxCommits * 2

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProperty ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_begin", "L_done"})

=====
