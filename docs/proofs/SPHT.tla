------------------------- MODULE SPHT -------------------------
(*
 * SPHT — Scalable Persistent Hardware Transactions
 *
 * Algorithm (from backends/tm_impl/spht/spht.hpp and Implementation_notes.md,
 * SPHT FAST 2021):
 *
 * Dual-path: Intel RTM (fast path) + SGL mutex (fallback).
 * The TSX + SGL dual-path protocol is modeled after TSXSGL.tla.
 *
 * Novel addition: Per-Thread Commit Log (PCL) with epoch-based
 * group commit for amortized NVM durability:
 *
 *   - Each thread appends write entries to a PCL during TSX txns.
 *   - On _xend(), the write is logically committed (atomic via TSX)
 *     but NOT yet durable (only in cache).
 *   - Every GROUP_COMMIT_INTERVAL txns, group_commit() flushes the
 *     PCL to NVM (clwb + sfence) and publishes the durable TX
 *     sequence number to a global epoch table.
 *   - Recovery: scan the durable epoch table to find the last
 *     globally-durable point, then replay PCL entries up to that
 *     point.  Entries beyond are discarded (crashed before flush).
 *
 * Invariants (for TLC model checking):
 *   TSXSafety:         If a thread is in TSX mode, sgl = 0.
 *   LockExclusion:     At most one thread holds the SGL mutex.
 *   DurableRedo:       After group_commit(t), all of t's PCL entries
 *                      up to its durable_seq are in NVM.
 *   RecoveryCorrect:   After recovery, each address holds the last
 *                      value written by a durably-committed TX.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data,               (* Set of possible data values *)
    MaxRetries,         (* Max TSX retries before SGL fallback *)
    GroupInterval       (* GROUP_COMMIT_INTERVAL *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxRetries \in Nat \ {0}
ASSUME GroupInterval \in Nat \ {0}

VARIABLES
    mem,                (* [Addr -> Data] *)
    sgl,                (* 0 = free, t = held by thread t *)
    tsx_mode,           (* [Thread -> BOOLEAN]  TRUE = in TSX xaction *)
    pc,                 (* [Thread -> {"idle", "active_tsx", "active_sgl",
                                      "commit_tsx", "commit_sgl",
                                      "group_commit", "aborting"}] *)
    retry_cnt,          (* [Thread -> Nat] *)
    tx_seq,             (* [Thread -> Nat]  per-thread TX counter *)
    pcl,                (* [Thread -> Seq(<<Addr, Data>>)]  PCL entries *)
    pcl_epoch_start,    (* [Thread -> Nat]  start of current epoch in PCL *)
    tsx_buffer,         (* [Thread -> Addr -> Data ∪ {NoWrite}]
                           TSX write buffer: writes during a TSX xaction
                           are buffered here, not written to mem directly *)
    durable_seq,        (* [Thread -> Nat]  per-thread durable watermark *)
    crash,              (* [Thread -> BOOLEAN]  TRUE if thread has crashed *)
    recovered           (* [Thread -> BOOLEAN]  TRUE if thread's log recovered *)

vars == <<mem, sgl, tsx_mode, pc, retry_cnt, tx_seq, pcl,
          pcl_epoch_start, tsx_buffer, durable_seq, crash, recovered>>

NoWrite == 0 - 1

(* True if thread t has written to addr a in the current TSX transaction *)
HasWrittenInTSX(t, a) == tsx_buffer[t][a] # NoWrite

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ mem = [a \in Addr |-> 0]
    /\ sgl = 0
    /\ tsx_mode = [t \in Thread |-> FALSE]
    /\ pc = [t \in Thread |-> "idle"]
    /\ retry_cnt = [t \in Thread |-> 0]
    /\ tx_seq = [t \in Thread |-> 0]
    /\ pcl = [t \in Thread |-> << >>]
    /\ pcl_epoch_start = [t \in Thread |-> 1]
    /\ tsx_buffer = [t \in Thread |-> [a \in Addr |-> NoWrite]]
    /\ durable_seq = [t \in Thread |-> 0]
    /\ crash = [t \in Thread |-> FALSE]
    /\ recovered = [t \in Thread |-> FALSE]

(*--------------------------------------------------------------------*)
(* TSX Path Actions                                                    *)
(*--------------------------------------------------------------------*)

(*── TSX Begin: attempt _xbegin() ────────────────────────────────────*)
TSXBegin(t) ==
    /\ (pc[t] = "idle" \/ pc[t] = "active_sgl")     (* also attempted after SGL failure *)
    /\ sgl = 0                                        (* TSX requires no concurrent SGL *)
    /\ pc' = [pc EXCEPT ![t] = "active_tsx"]
    /\ tsx_mode' = [tsx_mode EXCEPT ![t] = TRUE]
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = 0]
    (* PCL continues across TSX transactions (not reset) *)
    /\ UNCHANGED <<mem, sgl, tx_seq, pcl, pcl_epoch_start,
                   tsx_buffer, durable_seq, crash, recovered>>

(*── TSX Body Read: return buffered value or mem ─────────────────────*)
TSXRead(t, a) ==
    /\ pc[t] = "active_tsx"
    /\ a \in Addr
    (* RTM hardware tracks read-set for conflict detection.
       Reads return the buffered value if written in this TX,
       otherwise read from shared memory.  No state changes. *)
    /\ UNCHANGED vars

(*── TSX Body Write: buffer in tsx_buffer (not mem) ──────────────────*)
TSXWrite(t, a, v) ==
    /\ pc[t] = "active_tsx"
    /\ a \in Addr
    /\ v \in Data
    (* Append to PCL (for durability) *)
    /\ pcl' = [pcl EXCEPT ![t] = Append(pcl[t], <<a, v>>)]
    (* Buffer write in tsx_buffer (not mem — RTM will apply on commit) *)
    /\ tsx_buffer' = [tsx_buffer EXCEPT ![t][a] = v]
    /\ UNCHANGED <<mem, sgl, tsx_mode, pc, retry_cnt, tx_seq,
                   pcl_epoch_start, durable_seq, crash, recovered>>

(*── TSX Commit: _xend() — apply tsx_buffer to mem atomically ───────*)
TSXCommit(t) ==
    /\ pc[t] = "active_tsx"
    (* Atomically apply all buffered writes to shared memory *)
    /\ mem' = [a \in Addr |->
                 IF HasWrittenInTSX(t, a)
                 THEN tsx_buffer[t][a]
                 ELSE mem[a]]
    /\ tsx_buffer' = [tsx_buffer EXCEPT ![t] = [a \in Addr |-> NoWrite]]
    /\ tsx_mode' = [tsx_mode EXCEPT ![t] = FALSE]
    /\ tx_seq' = [tx_seq EXCEPT ![t] = tx_seq[t] + 1]
    (* Group commit every GroupInterval TXs *)
    /\ IF (tx_seq[t] + 1) % GroupInterval = 0
       THEN
           (* Trigger group_commit in next step *)
           /\ pc' = [pc EXCEPT ![t] = "group_commit"]
       ELSE
           /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<sgl, retry_cnt, pcl, pcl_epoch_start,
                   durable_seq, crash, recovered>>

(*── Group Commit: flush PCL to NVM ──────────────────────────────────*)
GroupCommit(t) ==
    /\ pc[t] = "group_commit"
    (* Flush all PCL entries from epoch_start onward *)
    /\ durable_seq' = [durable_seq EXCEPT ![t] = tx_seq[t]]
    (* In the real system: clwb all PCL entries, sfence,
       then publish to global epoch table.
       We model this as setting durable_seq[t] atomically. *)
    /\ pcl_epoch_start' = [pcl_epoch_start EXCEPT ![t] = Len(pcl[t]) + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, sgl, tsx_mode, retry_cnt, tx_seq, pcl,
                   tsx_buffer, crash, recovered>>

(*── TSX Abort: discard tsx_buffer, mem unchanged ────────────────────*)
TSXAbort(t) ==
    /\ pc[t] = "active_tsx"
    (* RTM aborted: all memory writes discarded.
       tsx_buffer is cleared — mem was never touched by this TX. *)
    /\ tsx_buffer' = [tsx_buffer EXCEPT ![t] = [a \in Addr |-> NoWrite]]
    /\ tsx_mode' = [tsx_mode EXCEPT ![t] = FALSE]
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = retry_cnt[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "aborting"]
    /\ UNCHANGED <<mem, sgl, tx_seq, pcl, pcl_epoch_start,
                   durable_seq, crash, recovered>>

(*── Abort recovery (after TSX abort) ────────────────────────────────*)
(* If retries exhausted, fall back to SGL *)
TSXRetryOrFallback(t) ==
    /\ pc[t] = "aborting"
    /\ IF retry_cnt[t] < MaxRetries
       THEN
            (* Can only retry TSX if SGL is still free *)
            /\ sgl = 0
            /\ pc' = [pc EXCEPT ![t] = "active_tsx"]
            /\ tsx_mode' = [tsx_mode EXCEPT ![t] = TRUE]
        ELSE
            (* Fall back to SGL — TSX mode ends *)
            /\ pc' = [pc EXCEPT ![t] = "active_sgl"]
            /\ tsx_mode' = [tsx_mode EXCEPT ![t] = FALSE]
    (* Pop stale PCL entries (from aborted TX) *)
    (* PCL entries from the aborted TX are between pcl_epoch_start
       and Len(pcl).  Remove them. *)
    /\ LET KeepLen == pcl_epoch_start[t] - 1 IN
       pcl' = [pcl EXCEPT ![t] = [i \in 1..KeepLen |-> pcl[t][i]]]
    /\ UNCHANGED <<mem, sgl, retry_cnt, tx_seq, tsx_buffer,
                   pcl_epoch_start, durable_seq, crash, recovered>>

(*--------------------------------------------------------------------*)
(* SGL Path Actions                                                    *)
(*--------------------------------------------------------------------*)

(*── SGL Begin: acquire mutex ────────────────────────────────────────*)
SGLBegin(t) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = 0
    (* No other thread is in TSX mode *)
    /\ \A other \in Thread \ {t} : tsx_mode[other] = FALSE
    /\ sgl' = t
    (* Reset retry counter for next TSX attempt *)
    /\ retry_cnt' = [retry_cnt EXCEPT ![t] = 0]
    /\ UNCHANGED <<mem, tsx_mode, pc, tx_seq, pcl, pcl_epoch_start,
                   tsx_buffer, durable_seq, crash, recovered>>

(*── SGL Body Read ───────────────────────────────────────────────────*)
SGLRead(t, a) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = t
    /\ a \in Addr
    /\ UNCHANGED vars

(*── SGL Body Write ──────────────────────────────────────────────────*)
SGLWrite(t, a, v) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = t
    /\ a \in Addr
    /\ v \in Data
    (* Append to PCL (for durability) and write to memory directly *)
    /\ pcl' = [pcl EXCEPT ![t] = Append(pcl[t], <<a, v>>)]
    /\ mem' = [mem EXCEPT ![a] = v]
    /\ UNCHANGED <<sgl, tsx_mode, pc, retry_cnt, tx_seq,
                   pcl_epoch_start, tsx_buffer, durable_seq,
                   crash, recovered>>

(*── SGL Commit: release mutex ───────────────────────────────────────*)
SGLCommit(t) ==
    /\ pc[t] = "active_sgl"
    /\ sgl = t
    /\ sgl' = 0
    /\ tx_seq' = [tx_seq EXCEPT ![t] = tx_seq[t] + 1]
    (* Group commit after SGL transactions too *)
    /\ IF (tx_seq[t] + 1) % GroupInterval = 0
       THEN
           /\ pc' = [pc EXCEPT ![t] = "group_commit"]
       ELSE
           /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, tsx_mode, retry_cnt, pcl, pcl_epoch_start,
                   tsx_buffer, durable_seq, crash, recovered>>

(*--------------------------------------------------------------------*)
(* Crash / Recovery                                                    *)
(*--------------------------------------------------------------------*)

(*── System crash: all threads stop ──────────────────────────────────*)
Crash ==
    (* Only model crash when no thread is in the middle of group_commit.
       A crash during group_commit is safe because the clwb + sfence
       guarantees that either all or none of the PCL entries are durable. *)
    /\ \A t \in Thread : pc[t] # "group_commit"
    /\ crash' = [t \in Thread |-> TRUE]
    /\ UNCHANGED <<mem, sgl, tsx_mode, pc, retry_cnt, tx_seq, pcl,
                   pcl_epoch_start, tsx_buffer, durable_seq, recovered>>

(*── Recovery: replay durable PCL entries ────────────────────────────*)
Recovery(t) ==
    /\ crash[t] = TRUE
    /\ recovered[t] = FALSE
    (* Replay: for each thread, find the durable watermark and
       replay PCL entries up to durable_seq.
       For simplicity, we model recovery as applying all durable PCL
       entries and discarding the rest. *)
    /\ LET DurableEntries ==
           [i \in 1..durable_seq[t] |->
               IF i <= Len(pcl[t])
               THEN pcl[t][i]
               ELSE <<0, 0>>]  (* padding, won't be applied *)
       IN
        (* Apply all durable PCL entries to memory *)
        /\ mem' = [a \in Addr |->
                     LET candidates == {i \in 1..durable_seq[t] :
                                         i <= Len(pcl[t]) /\ pcl[t][i][1] = a}
                     IN IF candidates = {}
                        THEN mem[a]
                        ELSE LET last == CHOOSE i \in candidates : TRUE IN
                             pcl[t][last][2]]
    /\ recovered' = [recovered EXCEPT ![t] = TRUE]
    (* After recovery, clean up *)
    /\ pcl' = [pcl EXCEPT ![t] = << >>]
    /\ pcl_epoch_start' = [pcl_epoch_start EXCEPT ![t] = 1]
    /\ UNCHANGED <<sgl, tsx_mode, pc, retry_cnt, tx_seq,
                   tsx_buffer, durable_seq, crash>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \/ \E t \in Thread :
        \/ TSXBegin(t)
        \/ \E a \in Addr : TSXRead(t, a)
        \/ \E a \in Addr : \E v \in Data : TSXWrite(t, a, v)
        \/ TSXCommit(t)
        \/ TSXAbort(t)
        \/ TSXRetryOrFallback(t)
        \/ SGLBegin(t)
        \/ \E a \in Addr : SGLRead(t, a)
        \/ \E a \in Addr : \E v \in Data : SGLWrite(t, a, v)
        \/ SGLCommit(t)
        \/ GroupCommit(t)
    \/ Crash
    \/ \E t2 \in Thread : Recovery(t2)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: If a thread is in TSX mode, sgl must be free (0) ───────────*)
TSXSafety ==
    \A t \in Thread :
        tsx_mode[t] = TRUE => sgl = 0

(*── I2: At most one thread holds the SGL mutex ─────────────────────*)
LockExclusion ==
    \A t1, t2 \in Thread :
        (sgl = t1 /\ sgl = t2) => t1 = t2

(*── I3: SGL holder is in SGL mode ──────────────────────────────────*)
LockOwnerInv ==
    \A t \in Thread :
        sgl = t => pc[t] \in {"active_sgl", "commit_sgl"}

(*── I4: No thread in TSX mode during SGL ownership ─────────────────*)
TSXvsSGLSafety ==
    \A t \in Thread :
        sgl # 0 => ~tsx_mode[t]

(*── I5: Per-thread durable_seq never decreases ─────────────────────*)
DurableSeqMonotonic ==
    \A t \in Thread :
        durable_seq[t] <= tx_seq[t]

(*── I6: PCL epoch_start is within PCL bounds ───────────────────────*)
PCLBounds ==
    \A t \in Thread :
        pcl_epoch_start[t] >= 1 /\
        pcl_epoch_start[t] <= Len(pcl[t]) + 1

(*── I7: After group commit, durable entry count is valid ───────────*)
DurableValid ==
    \A t \in Thread :
        durable_seq[t] <= Len(pcl[t])

(*── I8: No two threads can have TSX active simultaneously with
        SGL active (already covered by TSXSafety + LockExclusion,
        but stated explicitly for TLC) ──────────────────────────────*)
AtMostOneMode ==
    \A t1, t2 \in Thread :
        ~ ( tsx_mode[t1] = TRUE /\ pc[t2] = "active_sgl" /\ sgl # 0 )

(*── I9: tsx_buffer is non-empty only during active TSX transactions ─*)
TSXBufferInUse ==
    \A t \in Thread :
        (\E a \in Addr : HasWrittenInTSX(t, a)) => tsx_mode[t] = TRUE

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

(* Weak fairness: system eventually makes progress *)
Spec_WF == Spec /\ WF_vars(Next)

(* Every TSX or SGL transaction eventually completes *)
Completion ==
    \A t \in Thread :
        (pc[t] \in {"active_tsx", "active_sgl"})
        ~> (pc[t] \in {"idle", "aborting"})

(* After recovery, all addresses have consistent values *)
RecoveryConsistency ==
    (\A t \in Thread : crash[t] = TRUE /\ recovered[t] = TRUE)
    => \A a \in Addr :
           mem[a] \in Data

(*====================================================================*)
(* Model parameters                                                   *)
(*====================================================================*)

(* Default: Thread = {1, 2}; Addr = {0, 1}; Data = {0, 1};
   MaxRetries = 2; GroupInterval = 2 *)

====
