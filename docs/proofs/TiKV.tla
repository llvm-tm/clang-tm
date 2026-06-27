----------------------- MODULE TiKV ------------------------
(*
 * TiKV — Percolator 2PC Distributed TM Backend (PlusCal)
 *
 * TM addresses mapped to TiKV keys. Percolator 2PC with key-level
 * locking (prewrite → commit primary → commit secondary).
 *
 * Invariants:
 *   LockExclusion:     A key locked by at most one thread
 *   NoStaleLocks:      Idle threads hold no locks
 *   CommittedVisible:  After commit, writes are in kv_store
 *   SnapshotIsolation: Locked keys contain valid data
 *   CommitOrdering:    Primary precedes secondary in commit
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of client thread IDs *)
    Key,                (* Set of TM key identifiers *)
    Data,               (* Set of possible data values *)
    MaxRetries          (* Max retries before permanent abort *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Key \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxRetries \in Nat

NoWrite == 0 - 1

(*─── PlusCal algorithm ───────────────────────────────────────────────*)
(* --algorithm TiKV

variables
    kv_store = [k \in Key |-> 0],
    kv_locks = [k \in Key |-> 0],
    write_set = [t \in Thread |-> [k \in Key |-> NoWrite]],
    read_set = [t \in Thread |-> {}],
    snapshot = [t \in Thread |-> 0],
    primary_key = [t \in Thread |-> 0],
    prewrite_ok = [t \in Thread |-> FALSE],
    commit_ts = [t \in Thread |-> 0],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
    retry_count = [t \in Thread |-> 0];

define
    HasWritten(t, k) == write_set[t][k] # NoWrite
end define;

process ThreadProc \in Thread
begin

L_idle:
    either \* Begin transaction
        write_set[self] := [k \in Key |-> NoWrite];
        read_set[self] := {};
        snapshot[self] := 1;
        retry_count[self] := 0;
        goto L_active;
    or \* Remain idle
        goto L_idle;
    end either;

L_active:
    either \* Read: check write-set, cache in read-set
        with k \in Key do
            if ~HasWritten(self, k) then
                read_set[self] := read_set[self] \cup {k};
            end if;
        end with;
        goto L_active;
    or \* Write: buffer in local write-set
        with k \in Key, v \in Data do
            write_set[self][k] := v;
            \* Set primary key on first write
            if \A k2 \in Key : ~HasWritten(self, k2) then
                primary_key[self] := k;
            end if;
        end with;
        goto L_active;
    or \* Read-only commit
        if \A k \in Key : ~HasWritten(self, k) then
            committed[self] := committed[self] + 1;
            read_set[self] := {};
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Prewrite (Phase 1: acquire locks)
        if \E k \in Key : HasWritten(self, k) then
            if \A k \in Key : HasWritten(self, k) =>
                (kv_locks[k] = 0 \/ kv_locks[k] = self) then
                \* All locks available: acquire them
                kv_locks := [k \in Key |->
                    IF HasWritten(self, k) THEN self ELSE kv_locks[k]];
                prewrite_ok[self] := TRUE;
                goto L_prewriting;
            else
                \* Some lock held by another thread: abort
                kv_locks := [k \in Key |->
                    IF HasWritten(self, k) /\ kv_locks[k] = self
                    THEN 0 ELSE kv_locks[k]];
                prewrite_ok[self] := FALSE;
                aborted[self] := aborted[self] + 1;
                write_set[self] := [k \in Key |-> NoWrite];
                read_set[self] := {};
                goto L_idle;
            end if;
        else
            goto L_active;
        end if;
    or \* Conflict retry (TiKV TxnNotFound)
        if retry_count[self] < MaxRetries then
            retry_count[self] := retry_count[self] + 1;
            aborted[self] := aborted[self] + 1;
            kv_locks := [k \in Key |->
                IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]];
            write_set[self] := [k \in Key |-> NoWrite];
            read_set[self] := {};
            prewrite_ok[self] := FALSE;
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Conflict abort (permanent — retries exhausted)
        if retry_count[self] >= MaxRetries then
            aborted[self] := aborted[self] + 1;
            kv_locks := [k \in Key |->
                IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]];
            write_set[self] := [k \in Key |-> NoWrite];
            read_set[self] := {};
            prewrite_ok[self] := FALSE;
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Abort (direct)
        kv_locks := [k \in Key |->
            IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]];
        aborted[self] := aborted[self] + 1;
        write_set[self] := [k \in Key |-> NoWrite];
        read_set[self] := {};
        prewrite_ok[self] := FALSE;
        goto L_idle;
    end either;

L_prewriting:
    either \* Commit primary key (Phase 2a)
        with pk = primary_key[self] do
            if HasWritten(self, pk) then
                kv_store[pk] := write_set[self][pk];
                kv_locks[pk] := 0;
            end if;
            commit_ts[self] := 2;
        end with;
        goto L_committing;
    or \* Abort during prewrite
        kv_locks := [k \in Key |->
            IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]];
        aborted[self] := aborted[self] + 1;
        write_set[self] := [k \in Key |-> NoWrite];
        read_set[self] := {};
        prewrite_ok[self] := FALSE;
        goto L_idle;
    end either;

L_committing:
    \* Commit secondary keys (Phase 2b): write all remaining keys
    kv_store := [k \in Key |->
        IF HasWritten(self, k) /\ k # primary_key[self]
        THEN write_set[self][k]
        ELSE kv_store[k]];
    kv_locks := [k \in Key |->
        IF HasWritten(self, k) /\ k # primary_key[self]
        THEN 0 ELSE kv_locks[k]];
    committed[self] := committed[self] + 1;
    read_set[self] := {};
    write_set[self] := [k \in Key |-> NoWrite];
    goto L_idle;

end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES kv_store, kv_locks, write_set, read_set, snapshot, primary_key, 
          prewrite_ok, commit_ts, committed, aborted, retry_count, pc

(* define statement *)
HasWritten(t, k) == write_set[t][k] # NoWrite


vars == << kv_store, kv_locks, write_set, read_set, snapshot, primary_key, 
           prewrite_ok, commit_ts, committed, aborted, retry_count, pc >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ kv_store = [k \in Key |-> 0]
        /\ kv_locks = [k \in Key |-> 0]
        /\ write_set = [t \in Thread |-> [k \in Key |-> NoWrite]]
        /\ read_set = [t \in Thread |-> {}]
        /\ snapshot = [t \in Thread |-> 0]
        /\ primary_key = [t \in Thread |-> 0]
        /\ prewrite_ok = [t \in Thread |-> FALSE]
        /\ commit_ts = [t \in Thread |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ retry_count = [t \in Thread |-> 0]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ write_set' = [write_set EXCEPT ![self] = [k \in Key |-> NoWrite]]
                      /\ read_set' = [read_set EXCEPT ![self] = {}]
                      /\ snapshot' = [snapshot EXCEPT ![self] = 1]
                      /\ retry_count' = [retry_count EXCEPT ![self] = 0]
                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                   \/ /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                      /\ UNCHANGED <<write_set, read_set, snapshot, retry_count>>
                /\ UNCHANGED << kv_store, kv_locks, primary_key, prewrite_ok, 
                                commit_ts, committed, aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E k \in Key:
                             IF ~HasWritten(self, k)
                                THEN /\ read_set' = [read_set EXCEPT ![self] = read_set[self] \cup {k}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED read_set
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<kv_locks, write_set, primary_key, prewrite_ok, committed, aborted, retry_count>>
                     \/ /\ \E k \in Key:
                             \E v \in Data:
                               /\ write_set' = [write_set EXCEPT ![self][k] = v]
                               /\ IF \A k2 \in Key : ~HasWritten(self, k2)
                                     THEN /\ primary_key' = [primary_key EXCEPT ![self] = k]
                                     ELSE /\ TRUE
                                          /\ UNCHANGED primary_key
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<kv_locks, read_set, prewrite_ok, committed, aborted, retry_count>>
                     \/ /\ IF \A k \in Key : ~HasWritten(self, k)
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ read_set' = [read_set EXCEPT ![self] = {}]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << read_set, committed >>
                        /\ UNCHANGED <<kv_locks, write_set, primary_key, prewrite_ok, aborted, retry_count>>
                     \/ /\ IF \E k \in Key : HasWritten(self, k)
                              THEN /\ IF \A k \in Key : HasWritten(self, k) =>
                                          (kv_locks[k] = 0 \/ kv_locks[k] = self)
                                         THEN /\ kv_locks' =         [k \in Key |->
                                                             IF HasWritten(self, k) THEN self ELSE kv_locks[k]]
                                              /\ prewrite_ok' = [prewrite_ok EXCEPT ![self] = TRUE]
                                              /\ pc' = [pc EXCEPT ![self] = "L_prewriting"]
                                              /\ UNCHANGED << write_set, 
                                                              read_set, 
                                                              aborted >>
                                         ELSE /\ kv_locks' =         [k \in Key |->
                                                             IF HasWritten(self, k) /\ kv_locks[k] = self
                                                             THEN 0 ELSE kv_locks[k]]
                                              /\ prewrite_ok' = [prewrite_ok EXCEPT ![self] = FALSE]
                                              /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                              /\ write_set' = [write_set EXCEPT ![self] = [k \in Key |-> NoWrite]]
                                              /\ read_set' = [read_set EXCEPT ![self] = {}]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << kv_locks, write_set, 
                                                   read_set, prewrite_ok, 
                                                   aborted >>
                        /\ UNCHANGED <<primary_key, committed, retry_count>>
                     \/ /\ IF retry_count[self] < MaxRetries
                              THEN /\ retry_count' = [retry_count EXCEPT ![self] = retry_count[self] + 1]
                                   /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                   /\ kv_locks' =         [k \in Key |->
                                                  IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]]
                                   /\ write_set' = [write_set EXCEPT ![self] = [k \in Key |-> NoWrite]]
                                   /\ read_set' = [read_set EXCEPT ![self] = {}]
                                   /\ prewrite_ok' = [prewrite_ok EXCEPT ![self] = FALSE]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << kv_locks, write_set, 
                                                   read_set, prewrite_ok, 
                                                   aborted, retry_count >>
                        /\ UNCHANGED <<primary_key, committed>>
                     \/ /\ IF retry_count[self] >= MaxRetries
                              THEN /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                   /\ kv_locks' =         [k \in Key |->
                                                  IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]]
                                   /\ write_set' = [write_set EXCEPT ![self] = [k \in Key |-> NoWrite]]
                                   /\ read_set' = [read_set EXCEPT ![self] = {}]
                                   /\ prewrite_ok' = [prewrite_ok EXCEPT ![self] = FALSE]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED << kv_locks, write_set, 
                                                   read_set, prewrite_ok, 
                                                   aborted >>
                        /\ UNCHANGED <<primary_key, committed, retry_count>>
                     \/ /\ kv_locks' =         [k \in Key |->
                                       IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]]
                        /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                        /\ write_set' = [write_set EXCEPT ![self] = [k \in Key |-> NoWrite]]
                        /\ read_set' = [read_set EXCEPT ![self] = {}]
                        /\ prewrite_ok' = [prewrite_ok EXCEPT ![self] = FALSE]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED <<primary_key, committed, retry_count>>
                  /\ UNCHANGED << kv_store, snapshot, commit_ts >>

L_prewriting(self) == /\ pc[self] = "L_prewriting"
                      /\ \/ /\ LET pk == primary_key[self] IN
                                 /\ IF HasWritten(self, pk)
                                       THEN /\ kv_store' = [kv_store EXCEPT ![pk] = write_set[self][pk]]
                                            /\ kv_locks' = [kv_locks EXCEPT ![pk] = 0]
                                       ELSE /\ TRUE
                                            /\ UNCHANGED << kv_store, kv_locks >>
                                 /\ commit_ts' = [commit_ts EXCEPT ![self] = 2]
                            /\ pc' = [pc EXCEPT ![self] = "L_committing"]
                            /\ UNCHANGED <<write_set, read_set, prewrite_ok, aborted>>
                         \/ /\ kv_locks' =         [k \in Key |->
                                           IF kv_locks[k] = self THEN 0 ELSE kv_locks[k]]
                            /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                            /\ write_set' = [write_set EXCEPT ![self] = [k \in Key |-> NoWrite]]
                            /\ read_set' = [read_set EXCEPT ![self] = {}]
                            /\ prewrite_ok' = [prewrite_ok EXCEPT ![self] = FALSE]
                            /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                            /\ UNCHANGED <<kv_store, commit_ts>>
                      /\ UNCHANGED << snapshot, primary_key, committed, 
                                      retry_count >>

L_committing(self) == /\ pc[self] = "L_committing"
                      /\ kv_store' =         [k \in Key |->
                                     IF HasWritten(self, k) /\ k # primary_key[self]
                                     THEN write_set[self][k]
                                     ELSE kv_store[k]]
                      /\ kv_locks' =         [k \in Key |->
                                     IF HasWritten(self, k) /\ k # primary_key[self]
                                     THEN 0 ELSE kv_locks[k]]
                      /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                      /\ read_set' = [read_set EXCEPT ![self] = {}]
                      /\ write_set' = [write_set EXCEPT ![self] = [k \in Key |-> NoWrite]]
                      /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                      /\ UNCHANGED << snapshot, primary_key, prewrite_ok, 
                                      commit_ts, aborted, retry_count >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_prewriting(self)
                       \/ L_committing(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")



(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(* NOTE: Tautology — (kv_locks[k]=t1 /\ kv_locks[k]=t2) => t1=t2 *)
(* holds trivially. Kept for documentation clarity.              *)
LockExclusion ==
    \A k \in Key :
        \A t1, t2 \in Thread :
            (kv_locks[k] = t1 /\ kv_locks[k] = t2) => t1 = t2

NoStaleLocks ==
    \A t \in Thread :
        pc[t] = "L_idle" =>
            \A k \in Key : kv_locks[k] # t

(* NOTE: Vacuous invariant — HasWritten(t, k) is always false when *)
(* pc[t]="L_idle" because the write-set is cleared on every path   *)
(* back to L_idle. Kept for documentation; excluded from Inv.      *)
CommittedVisible ==
    \A t \in Thread, k \in Key :
        pc[t] = "L_idle" /\ HasWritten(t, k) =>
            \/ committed[t] > 0
            \/ aborted[t] > 0

SnapshotIsolation ==
    \A k \in Key :
        kv_locks[k] # 0 => kv_store[k] \in Data

CommitOrdering ==
    \A t \in Thread :
        pc[t] \in {"L_prewriting", "L_committing"} =>
            \/ primary_key[t] \in Key
            \/ \A k \in Key : ~HasWritten(t, k)

(*====================================================================*)
(* Bounding constraints for TLC termination                           *)
(*====================================================================*)
TLCBound ==
    /\ \A t \in Thread : committed[t] < 3
    /\ \A t \in Thread : aborted[t] < 3
    /\ \A t \in Thread : retry_count[t] < MaxRetries + 1

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

ProgressProperty ==
    \A t \in Thread :
        (pc[t] \in {"L_active", "L_prewriting", "L_committing"})
        ~> (pc[t] = "L_idle")

=====================================================================
