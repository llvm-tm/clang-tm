---------------------- MODULE LEFTRIGHT -----------------------
(*
 * LEFTRIGHT — Global-Clock OCC with Value-Based Validation (PlusCal)
 *
 * Despite the name, this is NOT Left-Right Synchronization.
 * It is a global-clock OCC with value-based validation (memcmp):
 *
 *   - g_clock: global version clock (monotonic counter)
 *   - g_commit_lock: spinlock serializing the commit path
 *   - Read-set: (addr, observed_clock, captured_value) triple
 *   - Write-set: (addr, new_value)
 *
 * Labels:
 *   L_idle         — begin new transaction or terminate
 *   L_begin        — capture snapshot, clear sets
 *   L_active       — non-deterministic: read, write, read-only commit,
 *                    or acquire lock
 *   L_validate     — under lock: check ver <= snapshot AND mem==captured
 *   L_inc_clock    — increment global clock
 *   L_write_back   — write buffered values to memory
 *   L_release_lock — release lock
 *   L_abort        — clean up (lock never held, or already released)
 *   L_done         — termination
 *)

EXTENDS Naturals, FiniteSets, TLC, TMTypes

CONSTANTS
    Thread,                (* Set of thread IDs *)
    Addr,                  (* Set of memory addresses *)
    Data,                  (* Set of possible data values *)
    MaxCommits             (* Max commits per thread for bounded model *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat \ {0}

(* ---- helpers ---- *)
(*--algorithm LEFTRIGHT

variables
    mem = [a \in Addr |-> 0],
    clock = 1,
    commit_lock = 0,
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
variables
    snapshot = 0,
    read_set = {},
    write_set = [a \in Addr |-> NoWrite],
    read_only = TRUE;
begin

L_idle:
    if committed[self] >= MaxCommits then
        goto L_done;
    else
        goto L_begin;
    end if;

L_begin:
    snapshot := clock;
    read_set := {};
    write_set := [a \in Addr |-> NoWrite];
    read_only := TRUE;
    lastSignalFence[self] := "";
    lastThreadFence[self] := "";
    lastRmw[self] := "";
    goto L_active;

L_active:
    either \* Read own write (no-op)
        with a \in Addr do
            if write_set[a] # NoWrite then skip; end if;
        end with;
        goto L_active;
    or \* Read (capture clock and value)
        with a \in Addr do
            if write_set[a] = NoWrite then
                read_set := read_set \union {<<a, clock, mem[a]>>};
            end if;
        end with;
        lastSignalFence[self] := "sc";
        goto L_active;
    or \* Write (no fence — relies on commit lock + clock increment for ordering)
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
        if ~read_only /\ commit_lock = 0 then
            commit_lock := self;
            lastRmw[self] := "acquire";
            goto L_validate;
        else
            goto L_active;
        end if;
    end either;

L_validate:
    (* Check: observed_ver <= snapshot AND value unchanged *)
    if \A <<a, ver, val>> \in read_set :
        ver <= snapshot /\ mem[a] = val
    then
        lastSignalFence[self] := "sc";
        goto L_inc_clock;
    else
        commit_lock := 0;
        lastRmw[self] := "release";
        goto L_abort;
    end if;

L_inc_clock:
    lastSignalFence[self] := "sc";
    clock := clock + 1;

L_write_back:
    mem := [a \in Addr |->
        IF write_set[a] # NoWrite THEN write_set[a] ELSE mem[a]];
    goto L_release_lock;

L_release_lock:
    lastRmw[self] := "release";
    commit_lock := 0;
    committed[self] := committed[self] + 1;
    goto L_idle;

L_abort:
    lastRmw[self] := "release";
    read_set := {};
    write_set := [a \in Addr |-> NoWrite];
    read_only := TRUE;
    snapshot := 0;
    aborted[self] := aborted[self] + 1;
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES pc, mem, clock, commit_lock, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, snapshot, read_set, 
          write_set, read_only

vars == << pc, mem, clock, commit_lock, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, snapshot, 
           read_set, write_set, read_only >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ clock = 1
        /\ commit_lock = 0
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastSignalFence = [self \in Thread |-> ""]
        /\ lastThreadFence = [self \in Thread |-> ""]
        /\ lastRmw = [self \in Thread |-> ""]
        (* Process ThreadProc *)
        /\ snapshot = [self \in Thread |-> 0]
        /\ read_set = [self \in Thread |-> {}]
        /\ write_set = [self \in Thread |-> [a \in Addr |-> NoWrite]]
        /\ read_only = [self \in Thread |-> TRUE]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_begin"]
                /\ UNCHANGED << mem, clock, commit_lock, committed, aborted, lastSignalFence, lastThreadFence, lastRmw,
                                snapshot, read_set, write_set, read_only >>

L_begin(self) == /\ pc[self] = "L_begin"
                 /\ snapshot' = [snapshot EXCEPT ![self] = clock]
                 /\ read_set' = [read_set EXCEPT ![self] = {}]
                 /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                 /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                 /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                 /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                 /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                 /\ pc' = [pc EXCEPT ![self] = "L_active"]
                 /\ UNCHANGED << mem, clock, commit_lock, committed, aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ (\/ /\ \E a \in Addr:
                              IF write_set[self][a] # NoWrite
                                 THEN /\ TRUE
                                 ELSE /\ TRUE
                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                         /\ UNCHANGED <<commit_lock, committed, read_set, write_set, read_only, lastSignalFence, lastThreadFence, lastRmw>>
                      \/ /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                         /\ \E a \in Addr:
                              IF write_set[self][a] = NoWrite
                                 THEN /\ read_set' = [read_set EXCEPT ![self] = read_set[self] \union {<<a, clock, mem[a]>>}]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED read_set
                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                         /\ UNCHANGED <<commit_lock, committed, write_set, read_only>>
                       /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                       /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                       /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                          /\ \E a \in Addr:
                               \E v \in Data:
                                 /\ write_set' = [write_set EXCEPT ![self][a] = v]
                                 /\ read_only' = [read_only EXCEPT ![self] = FALSE]
                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                         /\ UNCHANGED <<commit_lock, committed, read_set>>
                      \/ /\ IF read_only[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED committed
                         /\ UNCHANGED <<commit_lock, read_set, write_set, read_only, lastSignalFence, lastThreadFence, lastRmw>>
                      \/ /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                         /\ IF ~read_only[self] /\ commit_lock = 0
                              THEN /\ commit_lock' = self
                                   /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED commit_lock
                         /\ UNCHANGED <<committed, read_set, write_set, read_only, lastSignalFence, lastThreadFence>>)
                  /\ UNCHANGED << mem, clock, aborted, snapshot >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF \A <<a, ver, val>> \in read_set[self] :
                           ver <= snapshot[self] /\ mem[a] = val
                          THEN /\ pc' = [pc EXCEPT ![self] = "L_inc_clock"]
                               /\ UNCHANGED commit_lock
                               /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                          ELSE /\ commit_lock' = 0
                               /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                    /\ UNCHANGED << mem, clock, committed, aborted, snapshot, 
                                    read_set, write_set, read_only >>

L_inc_clock(self) == /\ pc[self] = "L_inc_clock"
                     /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                     /\ clock' = clock + 1
                     /\ pc' = [pc EXCEPT ![self] = "L_write_back"]
                     /\ UNCHANGED << mem, commit_lock, committed, aborted, 
                                     snapshot, read_set, write_set, read_only >>

L_write_back(self) == /\ pc[self] = "L_write_back"
                      /\ mem' =    [a \in Addr |->
                                IF write_set[self][a] # NoWrite THEN write_set[self][a] ELSE mem[a]]
                      /\ pc' = [pc EXCEPT ![self] = "L_release_lock"]
                      /\ UNCHANGED << clock, commit_lock, committed, aborted, 
                                      snapshot, read_set, write_set, read_only, lastSignalFence, lastThreadFence, lastRmw >>

L_release_lock(self) == /\ pc[self] = "L_release_lock"
                        /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                        /\ commit_lock' = 0
                        /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED << mem, clock, aborted, snapshot, 
                                        read_set, write_set, read_only >>

L_abort(self) == /\ pc[self] = "L_abort"
                 /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                 /\ read_set' = [read_set EXCEPT ![self] = {}]
                 /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                 /\ read_only' = [read_only EXCEPT ![self] = TRUE]
                 /\ snapshot' = [snapshot EXCEPT ![self] = 0]
                 /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                 /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                 /\ UNCHANGED << mem, clock, commit_lock, committed >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << mem, clock, commit_lock, committed, aborted, 
                                snapshot, read_set, write_set, read_only, lastSignalFence, lastThreadFence, lastRmw >>

ThreadProc(self) == L_idle(self) \/ L_begin(self) \/ L_active(self)
                       \/ L_validate(self) \/ L_inc_clock(self)
                       \/ L_write_back(self) \/ L_release_lock(self)
                       \/ L_abort(self) \/ L_done(self)

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
        (commit_lock = t1 /\ commit_lock = t2) => t1 = t2

(* I2: Lock holder is in a valid commit phase *)
LockHolderCommitting ==
    \A t \in Thread :
        commit_lock = t =>
            pc[t] \in {"L_validate", "L_inc_clock", "L_write_back",
                        "L_release_lock"}

(* I3: No two threads are in commit phases simultaneously *)
AtMostOneCommitting ==
    \A t1, t2 \in Thread :
        t1 # t2 =>
            ~ ( pc[t1] \in {"L_validate", "L_inc_clock", "L_write_back",
                             "L_release_lock"}
              /\ pc[t2] \in {"L_validate", "L_inc_clock", "L_write_back",
                             "L_release_lock"} )

(* I4: Threads with non-empty write-set have issued a fence *)
FenceFidelityInst == FenceFidelityPA(Thread, write_set, lastSignalFence, lastThreadFence, lastRmw)

(* Combined invariant for TLC *)
Inv ==
    /\ LockExclusion
    /\ LockHolderCommitting
    /\ AtMostOneCommitting
    /\ FenceFidelityInst

(* Constraint for bounded model checking *)
ModelBound == clock <= 5 /\ \A t \in Thread : aborted[t] <= MaxCommits * 2

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProp ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_done"})

=====
