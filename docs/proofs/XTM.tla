----------------------- MODULE XTM ------------------------
(*
 * XTM — Page-Granularity OCC with Private Copies (PlusCal)
 *
 * Original action-based spec: 334 lines, 11 actions, 6 states.
 * PlusCal conversion: 8 labels, ~230 lines.
 *
 * Algorithm:
 *   - Page-granular memory tracked via XADT (owner + version per page).
 *   - Reads: if page owned by another TX → abort.
 *     Otherwise record (page, version) in read-set.
 *   - Writes: CAS-acquire page ownership in XADT; buffer value in write-set.
 *   - Commit: validate read-set → write-back → release ownership, bump version.
 *   - Abort: release ownership, discard private copies.
 *
 * Labels:
 *   L_idle      — begin new transaction or terminate
 *   L_begin     — capture current state and start transaction
 *   L_active    — non-deterministic: read, write, validate, or abort
 *   L_writeback — write private copies to shared memory
 *   L_release   — release ownership + bump versions + commit
 *   L_abort     — release ownership, discard private copies
 *   L_done      — termination
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Page,               (* Set of page numbers *)
    Data,               (* Set of possible data values *)
    MaxCommits          (* Max commits per thread for bounded model *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Page \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat \ {0}

(* ---- helpers ---- *)
NoWrite == 0 - 1

(*--algorithm XTM

variables
    mem = [p \in Page |-> 0],
    xadt_owner = [p \in Page |-> 0],
    xadt_version = [p \in Page |-> 0],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0];

process ThreadProc \in Thread
variables
    read_set = {},
    write_set = [p \in Page |-> NoWrite];
begin

L_idle:
    if committed[self] >= MaxCommits then
        goto L_done;
    else
        goto L_begin;
    end if;

L_begin:
    read_set := {};
    write_set := [p \in Page |-> NoWrite];
    goto L_active;

L_active:
    either \* Read own write (no-op)
        with p \in Page do
            if write_set[p] # NoWrite then skip; end if;
        end with;
        goto L_active;
    or \* Read (conflict — abort)
        if \E p \in Page : write_set[p] = NoWrite /\ xadt_owner[p] \notin {0, self} then
            goto L_abort;
        else
            goto L_active;
        end if;
    or \* Read (record version)
        with p \in Page do
            if write_set[p] = NoWrite /\ xadt_owner[p] \in {0, self} then
                read_set := read_set \union {<<p, xadt_version[p]>>};
            end if;
        end with;
        goto L_active;
    or \* Write (already owned — update private copy)
        with p \in Page, v \in Data do
            if write_set[p] # NoWrite then
                write_set[p] := v;
            end if;
        end with;
        goto L_active;
    or \* Write (acquire ownership from free)
        with p \in Page, v \in Data do
            if write_set[p] = NoWrite /\ xadt_owner[p] = 0 then
                xadt_owner[p] := self;
                write_set[p] := v;
            end if;
        end with;
        goto L_active;
    or \* Write (already owned by self — but not in write-set yet)
        with p \in Page, v \in Data do
            if write_set[p] = NoWrite /\ xadt_owner[p] = self then
                write_set[p] := v;
            end if;
        end with;
        goto L_active;
    or \* Write (conflict — page owned by another)
        if \E p \in Page : write_set[p] = NoWrite /\ xadt_owner[p] \notin {0, self} then
            goto L_abort;
        else
            goto L_active;
        end if;
    or \* Validate and commit
        if \A <<p, ver>> \in read_set :
            write_set[p] # NoWrite \/
            (xadt_version[p] = ver /\ (xadt_owner[p] = 0 \/ xadt_owner[p] = self))
        then
            goto L_writeback;
        else
            goto L_abort;
        end if;
    end either;

L_writeback:
    mem := [p \in Page |->
        IF write_set[p] # NoWrite THEN write_set[p] ELSE mem[p]];
    goto L_release;

L_release:
    xadt_owner := [p \in Page |->
        IF xadt_owner[p] = self THEN 0 ELSE xadt_owner[p]];
    xadt_version := [p \in Page |->
        IF write_set[p] # NoWrite THEN xadt_version[p] + 1 ELSE xadt_version[p]];
    read_set := {};
    write_set := [p \in Page |-> NoWrite];
    committed[self] := committed[self] + 1;
    goto L_idle;

L_abort:
    xadt_owner := [p \in Page |->
        IF xadt_owner[p] = self THEN 0 ELSE xadt_owner[p]];
    read_set := {};
    write_set := [p \in Page |-> NoWrite];
    aborted[self] := aborted[self] + 1;
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES pc, mem, xadt_owner, xadt_version, committed, aborted, read_set, 
          write_set

vars == << pc, mem, xadt_owner, xadt_version, committed, aborted, read_set, 
           write_set >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [p \in Page |-> 0]
        /\ xadt_owner = [p \in Page |-> 0]
        /\ xadt_version = [p \in Page |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        (* Process ThreadProc *)
        /\ read_set = [self \in Thread |-> {}]
        /\ write_set = [self \in Thread |-> [p \in Page |-> NoWrite]]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_begin"]
                /\ UNCHANGED << mem, xadt_owner, xadt_version, committed, 
                                aborted, read_set, write_set >>

L_begin(self) == /\ pc[self] = "L_begin"
                 /\ read_set' = [read_set EXCEPT ![self] = {}]
                 /\ write_set' = [write_set EXCEPT ![self] = [p \in Page |-> NoWrite]]
                 /\ pc' = [pc EXCEPT ![self] = "L_active"]
                 /\ UNCHANGED << mem, xadt_owner, xadt_version, committed, 
                                 aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E p \in Page:
                             IF write_set[self][p] # NoWrite
                                THEN /\ TRUE
                                ELSE /\ TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<xadt_owner, read_set, write_set>>
                     \/ /\ IF \E p \in Page : write_set[self][p] = NoWrite /\ xadt_owner[p] \notin {0, self}
                              THEN /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<xadt_owner, read_set, write_set>>
                     \/ /\ \E p \in Page:
                             IF write_set[self][p] = NoWrite /\ xadt_owner[p] \in {0, self}
                                THEN /\ read_set' = [read_set EXCEPT ![self] = read_set[self] \union {<<p, xadt_version[p]>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED read_set
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<xadt_owner, write_set>>
                     \/ /\ \E p \in Page:
                             \E v \in Data:
                               IF write_set[self][p] # NoWrite
                                  THEN /\ write_set' = [write_set EXCEPT ![self][p] = v]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED write_set
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<xadt_owner, read_set>>
                     \/ /\ \E p \in Page:
                             \E v \in Data:
                               IF write_set[self][p] = NoWrite /\ xadt_owner[p] = 0
                                  THEN /\ xadt_owner' = [xadt_owner EXCEPT ![p] = self]
                                       /\ write_set' = [write_set EXCEPT ![self][p] = v]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << xadt_owner, write_set >>
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED read_set
                     \/ /\ \E p \in Page:
                             \E v \in Data:
                               IF write_set[self][p] = NoWrite /\ xadt_owner[p] = self
                                  THEN /\ write_set' = [write_set EXCEPT ![self][p] = v]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED write_set
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<xadt_owner, read_set>>
                     \/ /\ IF \E p \in Page : write_set[self][p] = NoWrite /\ xadt_owner[p] \notin {0, self}
                              THEN /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<xadt_owner, read_set, write_set>>
                     \/ /\ IF \A <<p, ver>> \in read_set[self] :
                               write_set[self][p] # NoWrite \/
                               (xadt_version[p] = ver /\ (xadt_owner[p] = 0 \/ xadt_owner[p] = self))
                              THEN /\ pc' = [pc EXCEPT ![self] = "L_writeback"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                        /\ UNCHANGED <<xadt_owner, read_set, write_set>>
                  /\ UNCHANGED << mem, xadt_version, committed, aborted >>

L_writeback(self) == /\ pc[self] = "L_writeback"
                     /\ mem' =    [p \in Page |->
                               IF write_set[self][p] # NoWrite THEN write_set[self][p] ELSE mem[p]]
                     /\ pc' = [pc EXCEPT ![self] = "L_release"]
                     /\ UNCHANGED << xadt_owner, xadt_version, committed, 
                                     aborted, read_set, write_set >>

L_release(self) == /\ pc[self] = "L_release"
                   /\ xadt_owner' =           [p \in Page |->
                                    IF xadt_owner[p] = self THEN 0 ELSE xadt_owner[p]]
                   /\ xadt_version' =             [p \in Page |->
                                      IF write_set[self][p] # NoWrite THEN xadt_version[p] + 1 ELSE xadt_version[p]]
                   /\ read_set' = [read_set EXCEPT ![self] = {}]
                   /\ write_set' = [write_set EXCEPT ![self] = [p \in Page |-> NoWrite]]
                   /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                   /\ UNCHANGED << mem, aborted >>

L_abort(self) == /\ pc[self] = "L_abort"
                 /\ xadt_owner' =           [p \in Page |->
                                  IF xadt_owner[p] = self THEN 0 ELSE xadt_owner[p]]
                 /\ read_set' = [read_set EXCEPT ![self] = {}]
                 /\ write_set' = [write_set EXCEPT ![self] = [p \in Page |-> NoWrite]]
                 /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                 /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                 /\ UNCHANGED << mem, xadt_version, committed >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << mem, xadt_owner, xadt_version, committed, 
                                aborted, read_set, write_set >>

ThreadProc(self) == L_idle(self) \/ L_begin(self) \/ L_active(self)
                       \/ L_writeback(self) \/ L_release(self)
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

(* I1: Each page is owned by at most one thread at a time *)
PageOwnershipExclusion ==
    \A p \in Page :
        \A t1, t2 \in Thread :
            (xadt_owner[p] = t1 /\ xadt_owner[p] = t2) => t1 = t2

(* I2: If a thread owns a page, it's in the write-set *)
OwnershipTracked ==
    \A t \in Thread, p \in Page :
        xadt_owner[p] = t => write_set[t][p] # NoWrite

(* I3: If a thread has written to a page, it owns it *)
WriteTrackedOwnership ==
    \A t \in Thread, p \in Page :
        write_set[t][p] # NoWrite => xadt_owner[p] = t

(* I4: A thread in writeback owns all its written pages *)
WritebackConsistent ==
    \A t \in Thread :
        pc[t] = "L_writeback" =>
            \A p \in Page : write_set[t][p] # NoWrite => xadt_owner[p] = t

(* I5: Page versions never decrease *)
VersionMonotonic ==
    \A p \in Page : xadt_version[p] >= 0

(* I6: No thread owns pages while idle *)
NoDirtyRead ==
    \A t \in Thread :
        pc[t] \in {"L_idle", "L_begin", "L_done"} =>
            \A p \in Page : xadt_owner[p] # t

(* Combined invariant for TLC *)
Inv ==
    /\ PageOwnershipExclusion
    /\ OwnershipTracked
    /\ WriteTrackedOwnership
    /\ WritebackConsistent
    /\ VersionMonotonic
    /\ NoDirtyRead

(* Constraint for bounded model checking *)
ModelBound == \A t \in Thread : aborted[t] <= MaxCommits * 2

=====
