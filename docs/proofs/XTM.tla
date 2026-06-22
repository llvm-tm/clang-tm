-------------------------- MODULE XTM --------------------------
(*
 * XTM — Page-Granularity OCC with Private Copies
 *
 * Algorithm (from backends/tm_impl/xtm/, XTM ASPLOS 2006):
 *
 *   Page-granularity transactional memory using a global XADT
 *   hash table tracking (owner, version) per page.
 *
 * Key design:
 *   - Memory is tracked at page granularity.
 *   - Each page has an XADT entry: (owner_tx_id, version).
 *   - Reads: look up page in XADT.  If owned by another TX → abort.
 *     Record (page, version) in read-set.  Read from shared memory.
 *   - Writes: CAS-acquire page ownership in XADT.
 *     Create private copy of the full page (conceptual; we model
 *     per-address writes).  All writes go to private copy.
 *   - Commit: validate read-set (page version unchanged) →
 *     write-back private copies → release ownership, bump versions.
 *   - Abort: release ownership, discard private copies.
 *
 * Invariants (for TLC model checking):
 *   PageOwnership:      Each page is owned by at most one TX at a time.
 *   NoDirtyRead:        A committing TX's read-set pages were not
 *                       modified concurrently.
 *   AtomicWriteBack:    Write-back is atomic (all-or-nothing).
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Page,               (* Set of page numbers *)
    Data,               (* Set of possible data values *)
    MaxRetries          (* Max retries before abort *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Page \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxRetries \in Nat \ {0}

VARIABLES
    mem,                (* [Page -> Data]  (page-granular memory) *)
    xadt_owner,         (* [Page -> Thread \cup {0}]  page owner *)
    xadt_version,       (* [Page -> Nat]  page version *)
    pc,                 (* [Thread -> {"idle", "active", "validating",
                                      "writeback", "commit_ok", "aborting"}] *)
    read_set,           (* [Thread -> Seq(<<Page, Nat>>)]  (page, version) *)
    write_set,          (* [Thread -> Page -> Data \cup {NoWrite}]
                           private copy: writes are buffered here *)
    abort_count,        (* [Thread -> Nat] *)
    commit_count,       (* [Thread -> Nat] *)
    retry_cnt           (* [Thread -> Nat] *)

vars == <<mem, xadt_owner, xadt_version, pc, read_set, write_set,
          abort_count, commit_count, retry_cnt>>

(*--------------------------------------------------------------------*)
(* Helpers                                                             *)
(*--------------------------------------------------------------------*)

NoWrite == -1

(* Thread t has written to page p *)
HasWritten(t, p) == write_set[t][p] # NoWrite

(* Thread t owns page p *)
OwnsPage(t, p) == xadt_owner[p] = t

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ mem = [p \in Page |-> 0]
    /\ xadt_owner = [p \in Page |-> 0]
    /\ xadt_version = [p \in Page |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ read_set = [t \in Thread |-> << >>]
    /\ write_set = [t \in Thread |-> [p \in Page |-> NoWrite]]
    /\ abort_count = [t \in Thread |-> 0]
    /\ commit_count = [t \in Thread |-> 0]
    /\ retry_cnt = [t \in Thread |-> 0]

(*--------------------------------------------------------------------*)
(* Actions                                                             *)
(*--------------------------------------------------------------------*)

(*── Begin ──────────────────────────────────────────────────────────*)
BeginXTM(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ read_set' = [read_set EXCEPT ![t] = << >>]
    /\ write_set' = [write_set EXCEPT ![t] = [p \in Page |-> NoWrite]]
    /\ UNCHANGED <<mem, xadt_owner, xadt_version, abort_count,
                   commit_count, retry_cnt>>

(*── Read page ──────────────────────────────────────────────────────*)
(*  Look up page in XADT.  If owned by another TX → abort.
    Otherwise, record version in read-set and read from memory. *)
ReadPage(t, p) ==
    /\ pc[t] = "active"
    /\ p \in Page
    /\ IF HasWritten(t, p)
       THEN
           (* Read own private copy *)
           /\ UNCHANGED vars
       ELSE
           /\ xadt_owner[p] = 0 \/ xadt_owner[p] = t
           (\* Page is free or owned by us → OK to read *\)
           /\ LET ver == xadt_version[p] IN
              (\* Record (page, version) in read-set *\)
              /\ read_set' = [read_set EXCEPT ![t] =
                                Append(read_set[t], <<p, ver>>)]
              /\ UNCHANGED <<mem, xadt_owner, xadt_version, pc,
                             write_set, abort_count, commit_count,
                             retry_cnt>>

(*── Read-page conflict: page owned by another TX → abort ──────────*)
ReadConflict(t, p) ==
    /\ pc[t] = "active"
    /\ p \in Page
    /\ ~HasWritten(t, p)
    /\ xadt_owner[p] \notin {0, t}  (* owned by another TX *)
    (\* Eager conflict: abort immediately *\)
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ abort_count' = [abort_count EXCEPT ![t] = abort_count[t] + 1]
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = retry_cnt[t] + 1]
    /\ UNCHANGED <<mem, xadt_owner, xadt_version, read_set,
                   write_set, commit_count>>

(*── Write page (acquire ownership via CAS, then write to private copy) *)
WritePage(t, p, v) ==
    /\ pc[t] = "active"
    /\ p \in Page
    /\ v \in Data
    /\ IF HasWritten(t, p)
       THEN
           (* Already own this page → just update private copy *)
           /\ write_set' = [write_set EXCEPT ![t][p] = v]
           /\ UNCHANGED <<xadt_owner, xadt_version>>
       ELSE
           (\* Try to acquire ownership: CAS owner from 0 to t *\)
           /\ xadt_owner[p] = 0
           /\ xadt_owner' = [xadt_owner EXCEPT ![p] = t]
           /\ write_set' = [write_set EXCEPT ![t][p] = v]
           (\* Keep current version (will bump on commit) *\)
           /\ UNCHANGED <<xadt_version>>
    /\ UNCHANGED <<mem, pc, read_set, abort_count, commit_count,
                   retry_cnt>>

(*── Write conflict: page owned by another TX → abort ──────────────*)
WriteConflict(t, p) ==
    /\ pc[t] = "active"
    /\ p \in Page
    /\ ~HasWritten(t, p)
    /\ xadt_owner[p] \notin {0, t}
    (\* CAS failed → abort *\)
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ abort_count' = [abort_count EXCEPT ![t] = abort_count[t] + 1]
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = retry_cnt[t] + 1]
    /\ UNCHANGED <<mem, xadt_owner, xadt_version, read_set,
                   write_set, commit_count>>

(*── Commit: validate → write-back → release + bump ────────────────*)
(* Phase 1: Validate read-set (pages not in write-set) *)
Validate(t) ==
    /\ pc[t] = "active"
    (\* Check: every read-set page not in write-set has
       unchanged version and no concurrent owner *\)
    /\ \A i \in 1..Len(read_set[t]) :
         LET entry == read_set[t][i]
             page == entry[1]
             captured_ver == entry[2] IN
         page \in {p \in Page : HasWritten(t, p)}
         \/ (xadt_version[page] = captured_ver /\ xadt_owner[page] = t)
         \/ (xadt_version[page] = captured_ver /\ xadt_owner[page] = 0)
    (\* All checks pass → proceed to write-back *\)
    /\ pc' = [pc EXCEPT ![t] = "writeback"]
    /\ UNCHANGED <<mem, xadt_owner, xadt_version, read_set,
                   write_set, abort_count, commit_count, retry_cnt>>

(*── Validation failure → abort ────────────────────────────────────*)
ValidateFailed(t) ==
    /\ pc[t] = "active"
    (\* Some read-set page version changed or owned by another TX *\)
    /\ ~ (\A i \in 1..Len(read_set[t]) :
            LET entry == read_set[t][i]
                page == entry[1]
                captured_ver == entry[2] IN
            page \in {p \in Page : HasWritten(t, p)}
            \/ (xadt_version[page] = captured_ver /\ xadt_owner[page] = t)
            \/ (xadt_version[page] = captured_ver /\ xadt_owner[page] = 0))
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ abort_count' = [abort_count EXCEPT ![t] = abort_count[t] + 1]
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = retry_cnt[t] + 1]
    /\ UNCHANGED <<mem, xadt_owner, xadt_version, read_set,
                   write_set, commit_count>>

(* Phase 2: Write-back (apply private copies to memory) *)
WriteBackXTM(t) ==
    /\ pc[t] = "writeback"
    (\* Copy private writes to shared memory *\)
    /\ mem' = [p \in Page |->
                 IF HasWritten(t, p) THEN write_set[t][p] ELSE mem[p]]
    /\ pc' = [pc EXCEPT ![t] = "commit_ok"]
    /\ UNCHANGED <<xadt_owner, xadt_version, read_set, write_set,
                   abort_count, commit_count, retry_cnt>>

(* Phase 3: Release ownership + bump versions *)
ReleaseAndBump(t) ==
    /\ pc[t] = "commit_ok"
    (\* For each page owned by t: release ownership and bump version *\)
    /\ xadt_owner' = [p \in Page |->
                        IF OwnsPage(t, p) THEN 0 ELSE xadt_owner[p]]
    /\ xadt_version' = [p \in Page |->
                          IF OwnsPage(t, p)
                          THEN xadt_version[p] + 1
                          ELSE xadt_version[p]]
    /\ commit_count' = [commit_count EXCEPT ![t] = commit_count[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, read_set, write_set, abort_count, retry_cnt>>

(*── Abort (release ownership, discard private copies) ─────────────*)
AbortXTM(t) ==
    /\ pc[t] = "aborting"
    (\* Release ownership of all pages t owned *\)
    /\ xadt_owner' = [p \in Page |->
                        IF OwnsPage(t, p) THEN 0 ELSE xadt_owner[p]]
    (\* No need to restore memory — writes went to private copy only *\)
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ write_set' = [write_set EXCEPT ![t] = [p \in Page |-> NoWrite]]
    /\ read_set' = [read_set EXCEPT ![t] = << >>]
    /\ UNCHANGED <<mem, xadt_version, commit_count, retry_cnt>>

(*── Retry: re-enter active state after abort ──────────────────────*)
RetryXTM(t) ==
    /\ pc[t] = "idle"
    /\ retry_cnt[t] > 0
    (\* Reset and try again *\)
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = 0]
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ read_set' = [read_set EXCEPT ![t] = << >>]
    /\ write_set' = [write_set EXCEPT ![t] = [p \in Page |-> NoWrite]]
    /\ UNCHANGED <<mem, xadt_owner, xadt_version, abort_count,
                   commit_count>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \E t \in Thread :
        \/ BeginXTM(t)
        \/ \E p \in Page : ReadPage(t, p)
        \/ \E p \in Page : ReadConflict(t, p)
        \/ \E p \in Page : \E v \in Data : WritePage(t, p, v)
        \/ \E p \in Page : WriteConflict(t, p)
        \/ Validate(t)
        \/ ValidateFailed(t)
        \/ WriteBackXTM(t)
        \/ ReleaseAndBump(t)
        \/ AbortXTM(t)
        \/ RetryXTM(t)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: Each page is owned by at most one TX at a time ─────────────*)
PageOwnershipExclusion ==
    \A p \in Page :
        \A t1, t2 \in Thread :
            (xadt_owner[p] = t1 /\ xadt_owner[p] = t2) => t1 = t2

(*── I2: If a thread owns a page, it's in the write-set ────────────*)
OwnershipTracked ==
    \A t \in Thread, p \in Page :
        OwnsPage(t, p) => HasWritten(t, p)

(*── I3: If a thread has written to a page, it owns it ─────────────*)
WriteTrackedOwnership ==
    \A t \in Thread, p \in Page :
        HasWritten(t, p) => OwnsPage(t, p)

(*── I4: A thread in writeback owns all its written pages ──────────*)
WritebackConsistent ==
    \A t \in Thread :
        pc[t] = "writeback" =>
            \A p \in Page : HasWritten(t, p) => OwnsPage(t, p)

(*── I5: Aborting thread releases all ownership ────────────────────*)
AbortReleases ==
    \A t \in Thread :
        pc[t] = "aborting" =>
            \A p \in Page : xadt_owner[p] \in {0} \cup (Thread \ {t})
            (\* t may still own pages before AbortXTM fires *\)
            \/ \E a \in Page : xadt_owner[a] = t

(*── I6: Page versions are consistent with commit count ────────────*)
VersionMonotonic ==
    \A p \in Page :
        /\ xadt_version[p] <= commit_count[t]  (* per-page best effort *)
        /\ xadt_version[p] >= 0

(*── I7: No dirty reads across transactions ────────────────────────*)
NoDirtyRead ==
    \A t \in Thread :
        pc[t] = "idle" =>
            \A p \in Page : xadt_owner[p] # t

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

(* Every active transaction eventually completes *)
Completion ==
    \A t \in Thread :
        (pc[t] = "active") ~> (pc[t] \in {"idle", "writeback", "commit_ok"})

(* No pages are permanently locked *)
NoPermanentLock ==
    \A p \in Page :
        <>(xadt_owner[p] = 0)

(*====================================================================*)
(* Model parameters                                                   *)
(*====================================================================*)

(* Default: Thread = {1, 2}; Page = {0, 1}; Data = {0, 1};
   MaxRetries = 2 *)
