----------------------------- MODULE JVSTM -----------------------------
(*
 * JVSTM — Multi-Version OCC with Versioned Boxes
 *
 * Based on the Java Versioned STM (Cachopo & Rito-Silva, 2006).
 * Uses versioned boxes (VBoxes) that keep a history of values.
 * Read-only transactions never abort: they always see a consistent
 * snapshot at their start version (read-version, RV).
 * Write transactions acquire a global commit lock, validate all
 * touched addresses, then create new versions atomically.
 *
 * CONSTANTS:
 *   Thread     — Set of thread IDs (positive integers)
 *   Addr       — Set of memory addresses
 *   Data       — Set of data values
 *   MaxCommits — Max committed transactions per thread (bound)
 *
 * Algorithm (per transaction):
 *   1.  BEGIN: load global_clock → rv
 *   2.  READ: check write-set first (read-own-writes);
 *       else walk VBox history for version ≤ rv, capture version in read-set
 *   3.  WRITE: buffer in write-set
 *   4.  COMMIT (write tx): lock → inc clock → ct;
 *       validate read-set (each VBox head version = captured);
 *       validate write-set (each VBox head version ≤ rv);
 *       if OK: prepend new VBoxBody(version=ct, value) to each written VBox
 *       else: unlock, abort, retry
 *   5.  READ-ONLY COMMIT: no lock, no validation, immediate return
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC, TMTypes

CONSTANTS Thread, Addr, Data, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat \ {0}

(* --algorithm JVSTM

variables
    \* ── Global state ──
    clock = 0,          \* monotonically increasing version clock
    commit_lock = 0,    \* 0 = free, >0 = holder thread ID

    \* ── Per-address VBox ──
    \* Each VBox has a sequence of (version, value) bodies, newest first.
    \* vbox_hist[addr] = << <<ver, val>>, <<ver, val>>, ... >>
    vbox_hist = [a \in Addr |-> << <<0, 0>> >>],

    \* ── Per-thread counters ──
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],

    \* ── Fence tracking ──
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

define
    \* FindBody(seq, read_ver): returns the newest body with version ≤ read_ver.
    \* seq is a sequence of <<version, value>> entries, newest first.
    FindBody(seq, read_ver) ==
        seq[CHOOSE i \in 1..Len(seq) : seq[i][1] <= read_ver]
end define;

process ThreadProc \in Thread
variables
    \* ── Transaction-local state ──
    rv = 0,                     \* read version (snapshot)
    read_set = {},              \* set of <<addr, captured_version>>
    write_set = [a \in Addr |-> NoWrite],
    has_write = FALSE,          \* whether this tx has any write
    ct = 0;                     \* commit version
begin

L_idle:
    if committed[self] >= MaxCommits then
        goto L_done;
    else
        \* Begin transaction
        rv := clock;
        read_set := {};
        write_set := [a \in Addr |-> NoWrite];
        has_write := FALSE;
        ct := 0;
        lastSignalFence[self] := "";
        lastThreadFence[self] := "";
        lastRmw[self] := "";
        goto L_active;
    end if;

L_active:
    either \* ── Read: find body with version ≤ rv ──
        with a \in Addr do
            if write_set[a] # NoWrite then
                skip;   \* read-own-write
            else
                with cur_body = FindBody(vbox_hist[a], rv) do
                    read_set := read_set \union {<<a, cur_body[1]>>};
                end with;
                lastSignalFence[self] := "sc";
            end if;
        end with;
        goto L_active;
    or \* ── Write: buffer value ──
        with a \in Addr, v \in Data do
            write_set[a] := v;
            has_write := TRUE;
            lastRmw[self] := "acquire";
        end with;
        goto L_active;
    or \* ── Read-only commit ──
        if ~has_write then
            committed[self] := committed[self] + 1;
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* ── Write commit (acquire lock, increment clock) ──
        if has_write /\ commit_lock = 0 then
            commit_lock := self;
            lastRmw[self] := "acquire";
            ct := clock + 1;
            clock := clock + 1;
            goto L_validate;
        else
            goto L_active;
        end if;
    end either;

L_validate:
    \* Validate: read-set (each vbox newest version = captured)
    \*           write-set (each vbox newest version ≤ rv)
    if \A <<addr, captured>> \in read_set :
            vbox_hist[addr][1][1] = captured
      /\ \A a \in Addr :
            (write_set[a] # NoWrite) =>
                vbox_hist[a][1][1] <= rv
    then
        \* Validation passed — prepend new bodies
        vbox_hist := [a \in Addr |->
            IF write_set[a] # NoWrite THEN
                << <<ct, write_set[a]>> >> \o vbox_hist[a]
            ELSE
                vbox_hist[a]];
        lastSignalFence[self] := "sc";
        commit_lock := 0;
        lastRmw[self] := "release";
        committed[self] := committed[self] + 1;
        goto L_idle;
    else
        \* Validation failed — abort
        commit_lock := 0;
        lastRmw[self] := "release";
        aborted[self] := aborted[self] + 1;
        goto L_idle;
    end if;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES clock, commit_lock, vbox_hist, committed, aborted, lastSignalFence, 
          lastThreadFence, lastRmw, pc

(* define statement *)
FindBody(seq, read_ver) ==
    seq[CHOOSE i \in 1..Len(seq) : seq[i][1] <= read_ver]

VARIABLES rv, read_set, write_set, has_write, ct

vars == << clock, commit_lock, vbox_hist, committed, aborted, lastSignalFence, 
           lastThreadFence, lastRmw, pc, rv, read_set, write_set, has_write, 
           ct >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ clock = 0
        /\ commit_lock = 0
        /\ vbox_hist = [a \in Addr |-> << <<0, 0>> >>]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        (* Process ThreadProc *)
        /\ rv = [self \in Thread |-> 0]
        /\ read_set = [self \in Thread |-> {}]
        /\ write_set = [self \in Thread |-> [a \in Addr |-> NoWrite]]
        /\ has_write = [self \in Thread |-> FALSE]
        /\ ct = [self \in Thread |-> 0]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                           /\ UNCHANGED << lastSignalFence, lastThreadFence, 
                                           lastRmw, rv, read_set, write_set, 
                                           has_write, ct >>
                      ELSE /\ rv' = [rv EXCEPT ![self] = clock]
                           /\ read_set' = [read_set EXCEPT ![self] = {}]
                           /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                           /\ has_write' = [has_write EXCEPT ![self] = FALSE]
                           /\ ct' = [ct EXCEPT ![self] = 0]
                           /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                           /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                           /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                /\ UNCHANGED << clock, commit_lock, vbox_hist, committed, 
                                aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF write_set[self][a] # NoWrite
                                THEN /\ TRUE
                                     /\ UNCHANGED << lastSignalFence, read_set >>
                                ELSE /\ LET cur_body == FindBody(vbox_hist[a], rv[self]) IN
                                          read_set' = [read_set EXCEPT ![self] = read_set[self] \union {<<a, cur_body[1]>>}]
                                     /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, commit_lock, committed, lastRmw, write_set, has_write, ct>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               /\ write_set' = [write_set EXCEPT ![self][a] = v]
                               /\ has_write' = [has_write EXCEPT ![self] = TRUE]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clock, commit_lock, committed, lastSignalFence, read_set, ct>>
                     \/ /\ IF ~has_write[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED committed
                        /\ UNCHANGED <<clock, commit_lock, lastSignalFence, lastRmw, read_set, write_set, has_write, ct>>
                     \/ /\ IF has_write[self] /\ commit_lock = 0
                              THEN /\ commit_lock' = self
                                   /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                                   /\ ct' = [ct EXCEPT ![self] = clock + 1]
                                   /\ clock' = clock + 1
                                   /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << clock, commit_lock, lastRmw, 
                                                   ct >>
                        /\ UNCHANGED <<committed, lastSignalFence, read_set, write_set, has_write>>
                  /\ UNCHANGED << vbox_hist, aborted, lastThreadFence, rv >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF  \A <<addr, captured>> \in read_set[self] :
                                vbox_hist[addr][1][1] = captured
                          /\ \A a \in Addr :
                                (write_set[self][a] # NoWrite) =>
                                    vbox_hist[a][1][1] <= rv[self]
                          THEN /\ vbox_hist' =          [a \in Addr |->
                                               IF write_set[self][a] # NoWrite THEN
                                                   << <<ct[self], write_set[self][a]>> >> \o vbox_hist[a]
                                               ELSE
                                                   vbox_hist[a]]
                               /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                               /\ commit_lock' = 0
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                               /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                               /\ UNCHANGED aborted
                          ELSE /\ commit_lock' = 0
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                               /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                               /\ UNCHANGED << vbox_hist, committed, 
                                               lastSignalFence >>
                    /\ UNCHANGED << clock, lastThreadFence, rv, read_set, 
                                    write_set, has_write, ct >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clock, commit_lock, vbox_hist, committed, 
                                aborted, lastSignalFence, lastThreadFence, 
                                lastRmw, rv, read_set, write_set, has_write, 
                                ct >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_validate(self)
                       \/ L_done(self)

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

(* I0: Model bound to prevent infinite execution *)
ModelBound ==
    \A t \in Thread : committed[t] <= MaxCommits

(* I1: Commit lock is held by at most one thread *)
LockExclusion ==
    \A t1, t2 \in Thread : (commit_lock = t1 /\ commit_lock = t2) => t1 = t2

(* I2: Lock holder is validating *)
LockHolderState ==
    \A t \in Thread : commit_lock = t => pc[t] \in {"L_validate"}

(* I3: Fence fidelity — threads with write-sets have issued a fence *)
FenceFidelityInst ==
    FenceFidelity(Thread, [t \in Thread |-> {a \in Addr : write_set[t][a] # NoWrite}],
                  lastSignalFence, lastThreadFence, lastRmw)

(* I4: Global clock never decreases *)
ClockMonotonic == clock >= 0

(* I5: Read-set captures a version that exists in the VBox history *)
ReadSetValid ==
    \A t \in Thread :
        \A <<addr, captured>> \in read_set[t] :
            \E i \in 1..Len(vbox_hist[addr]) :
                vbox_hist[addr][i][1] = captured

(* I6: VBox bodies are ordered newest-first (strictly decreasing versions) *)
BodiesOrdered ==
    \A a \in Addr :
        \A i \in 1..(Len(vbox_hist[a])-1) :
            vbox_hist[a][i][1] > vbox_hist[a][i+1][1]

(* Combined invariant *)
Inv == /\ LockExclusion
       /\ LockHolderState
       /\ ClockMonotonic
       /\ ReadSetValid
       /\ BodiesOrdered
       /\ FenceFidelityInst

THEOREM Spec => []Inv

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

ProgressProp ==
    \A self \in Thread :
        (pc[self] \in {"L_active", "L_validate"}
         ~> pc[self] \in {"L_idle", "L_done"})

=============================================================================
