------------------------- MODULE SPHT -------------------------
(*
 * SPHT — Scalable Persistent Hardware Transactions (PlusCal)
 *
 * Algorithm (from backends/tm_impl/spht/spht.hpp):
 *
 * Dual-path: Intel RTM (fast path) + SGL mutex (fallback).
 * Per-Thread Commit Log (PCL) with epoch-based group commit.
 *
 * Invariants:
 *   TSXSafety:         tsx_mode[t]=TRUE => sgl=0
 *   LockExclusion:     At most one thread holds sgl
 *   TSXvsSGLSafety:    sgl#0 => ~tsx_mode[t]
 *   DurableSeqMonotonic: durable_seq[t] <= tx_seq[t]
 *   PCLBounds:         pcl_epoch_start[t] between 1 and Len(pcl[t])+1
 *   AtMostOneMode:     No concurrent TSX + SGL
 *   TSXBufferInUse:    tsx_buffer non-empty => tsx_mode[t]
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC, TMTypes

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

(*─── PlusCal algorithm ───────────────────────────────────────────────*)
(* --algorithm SPHT

variables
    mem = [a \in Addr |-> 0],
    sgl = 0,
    tsx_mode = [t \in Thread |-> FALSE],
    retry_cnt = [t \in Thread |-> 0],
    tx_seq = [t \in Thread |-> 0],
    pcl = [t \in Thread |-> << >>],
    pcl_epoch_start = [t \in Thread |-> 1],
    tsx_buffer = [t \in Thread |-> [a \in Addr |-> NoWrite]],
    durable_seq = [t \in Thread |-> 0],
    crashed = [t \in Thread |-> FALSE],
    recovered = [t \in Thread |-> FALSE];

process ThreadProc \in Thread
begin

L_idle:
    if crashed[self] /\ ~recovered[self] then
        \* Recovery: apply durable PCL entries to memory
        mem := [a \in Addr |->
            LET candidates == {i \in 1..durable_seq[self] :
                               i <= Len(pcl[self]) /\ pcl[self][i][1] = a}
            IN IF candidates = {}
               THEN mem[a]
               ELSE pcl[self][CHOOSE i \in candidates : TRUE][2]];
        tsx_buffer[self] := [a \in Addr |-> NoWrite];
        pcl[self] := << >>;
        pcl_epoch_start[self] := 1;
        recovered[self] := TRUE;
        goto L_idle;
    elsif recovered[self] /\ crashed[self] then
        goto L_idle;
    else
        either \* Try TSX begin
            await sgl = 0;
            tsx_mode[self] := TRUE;
            retry_cnt[self] := 0;
            goto L_active_tsx;
        or \* Remain idle
            goto L_idle;
        end either;
    end if;

L_active_tsx:
    either \* Read (nop — RTM tracks read-set in hardware)
        goto L_active_tsx;
    or \* Write (buffer in tsx_buffer, append to PCL)
        with a \in Addr, v \in Data do
            tsx_buffer[self][a] := v;
            pcl[self] := Append(pcl[self], <<a, v>>);
        end with;
        goto L_active_tsx;
    or \* Commit (_xend): apply buffer atomically to memory
        mem := [a \in Addr |->
            IF tsx_buffer[self][a] # NoWrite
            THEN tsx_buffer[self][a]
            ELSE mem[a]];
        tsx_buffer[self] := [a \in Addr |-> NoWrite];
        tsx_mode[self] := FALSE;
        tx_seq[self] := tx_seq[self] + 1;
        if tx_seq[self] % GroupInterval = 0 then
            goto L_group_commit;
        else
            goto L_idle;
        end if;
    or \* Abort (nondeterministic: RTM may abort at any time)
        tsx_buffer[self] := [a \in Addr |-> NoWrite];
        tsx_mode[self] := FALSE;
        retry_cnt[self] := retry_cnt[self] + 1;
        goto L_aborting;
    end either;

L_aborting:
    \* Truncate PCL entries added during the aborted transaction
    with keep_len = pcl_epoch_start[self] - 1 do
        if keep_len > 0 then
            pcl[self] := [i \in 1..keep_len |-> pcl[self][i]];
        else
            pcl[self] := << >>;
        end if;
    end with;
    \* Decide: retry TSX or fall back to SGL
    if retry_cnt[self] < MaxRetries then
        await sgl = 0;
        tsx_mode[self] := TRUE;
        goto L_active_tsx;
    else
        goto L_active_sgl;
    end if;

L_active_sgl:
    either \* Try TSX once more (before acquiring SGL lock)
        await sgl = 0;
        tsx_mode[self] := TRUE;
        retry_cnt[self] := 0;
        goto L_active_tsx;
    or \* Acquire SGL lock
        await sgl = 0 /\ \A other \in Thread \ {self} : ~tsx_mode[other];
        sgl := self;
        retry_cnt[self] := 0;
        goto L_active_sgl_locked;
    end either;

L_active_sgl_locked:
    either \* Read (nop)
        goto L_active_sgl_locked;
    or \* Write (direct to memory, append to PCL)
        with a \in Addr, v \in Data do
            mem[a] := v;
            pcl[self] := Append(pcl[self], <<a, v>>);
        end with;
        goto L_active_sgl_locked;
    or \* Commit (release SGL lock)
        sgl := 0;
        tx_seq[self] := tx_seq[self] + 1;
        if tx_seq[self] % GroupInterval = 0 then
            goto L_group_commit;
        else
            goto L_idle;
        end if;
    end either;

L_group_commit:
    durable_seq[self] := tx_seq[self];
    pcl_epoch_start[self] := Len(pcl[self]) + 1;
    goto L_idle;

end process;

process CrashProc = 0
begin
L_crash:
    await \A t \in Thread : pc[t] # "L_group_commit";
    crashed := [t \in Thread |-> TRUE];
end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES mem, sgl, tsx_mode, retry_cnt, tx_seq, pcl, pcl_epoch_start, 
          tsx_buffer, durable_seq, crashed, recovered, pc

vars == << mem, sgl, tsx_mode, retry_cnt, tx_seq, pcl, pcl_epoch_start, 
           tsx_buffer, durable_seq, crashed, recovered, pc >>

ProcSet == (Thread) \cup {0}

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ sgl = 0
        /\ tsx_mode = [t \in Thread |-> FALSE]
        /\ retry_cnt = [t \in Thread |-> 0]
        /\ tx_seq = [t \in Thread |-> 0]
        /\ pcl = [t \in Thread |-> << >>]
        /\ pcl_epoch_start = [t \in Thread |-> 1]
        /\ tsx_buffer = [t \in Thread |-> [a \in Addr |-> NoWrite]]
        /\ durable_seq = [t \in Thread |-> 0]
        /\ crashed = [t \in Thread |-> FALSE]
        /\ recovered = [t \in Thread |-> FALSE]
        /\ pc = [self \in ProcSet |-> CASE self \in Thread -> "L_idle"
                                        [] self = 0 -> "L_crash"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF crashed[self] /\ ~recovered[self]
                      THEN /\ mem' =    [a \in Addr |->
                                     LET candidates == {i \in 1..durable_seq[self] :
                                                        i <= Len(pcl[self]) /\ pcl[self][i][1] = a}
                                     IN IF candidates = {}
                                        THEN mem[a]
                                        ELSE pcl[self][CHOOSE i \in candidates : TRUE][2]]
                           /\ pcl' = [pcl EXCEPT ![self] = << >>]
                           /\ pcl_epoch_start' = [pcl_epoch_start EXCEPT ![self] = 1]
                           /\ recovered' = [recovered EXCEPT ![self] = TRUE]
                           /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                           /\ UNCHANGED << tsx_mode, retry_cnt >>
                      ELSE /\ IF recovered[self] /\ crashed[self]
                                 THEN /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                      /\ UNCHANGED << tsx_mode, retry_cnt >>
                                 ELSE /\ \/ /\ sgl = 0
                                            /\ tsx_mode' = [tsx_mode EXCEPT ![self] = TRUE]
                                            /\ retry_cnt' = [retry_cnt EXCEPT ![self] = 0]
                                            /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                                         \/ /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                            /\ UNCHANGED <<tsx_mode, retry_cnt>>
                           /\ UNCHANGED << mem, pcl, pcl_epoch_start, 
                                           recovered >>
                /\ UNCHANGED << sgl, tx_seq, tsx_buffer, durable_seq, crashed >>

L_active_tsx(self) == /\ pc[self] = "L_active_tsx"
                      /\ \/ /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                            /\ UNCHANGED <<mem, tsx_mode, retry_cnt, tx_seq, pcl, tsx_buffer>>
                         \/ /\ \E a \in Addr:
                                 \E v \in Data:
                                   /\ tsx_buffer' = [tsx_buffer EXCEPT ![self][a] = v]
                                   /\ pcl' = [pcl EXCEPT ![self] = Append(pcl[self], <<a, v>>)]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                            /\ UNCHANGED <<mem, tsx_mode, retry_cnt, tx_seq>>
                         \/ /\ mem' =    [a \in Addr |->
                                      IF tsx_buffer[self][a] # NoWrite
                                      THEN tsx_buffer[self][a]
                                      ELSE mem[a]]
                            /\ tsx_buffer' = [tsx_buffer EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                            /\ tsx_mode' = [tsx_mode EXCEPT ![self] = FALSE]
                            /\ tx_seq' = [tx_seq EXCEPT ![self] = tx_seq[self] + 1]
                            /\ IF tx_seq'[self] % GroupInterval = 0
                                  THEN /\ pc' = [pc EXCEPT ![self] = "L_group_commit"]
                                  ELSE /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                            /\ UNCHANGED <<retry_cnt, pcl>>
                         \/ /\ tsx_buffer' = [tsx_buffer EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                            /\ tsx_mode' = [tsx_mode EXCEPT ![self] = FALSE]
                            /\ retry_cnt' = [retry_cnt EXCEPT ![self] = retry_cnt[self] + 1]
                            /\ pc' = [pc EXCEPT ![self] = "L_aborting"]
                            /\ UNCHANGED <<mem, tx_seq, pcl>>
                      /\ UNCHANGED << sgl, pcl_epoch_start, durable_seq, 
                                      crashed, recovered >>

L_aborting(self) == /\ pc[self] = "L_aborting"
                    /\ LET keep_len == pcl_epoch_start[self] - 1 IN
                         IF keep_len > 0
                            THEN /\ pcl' = [pcl EXCEPT ![self] = [i \in 1..keep_len |-> pcl[self][i]]]
                            ELSE /\ pcl' = [pcl EXCEPT ![self] = << >>]
                    /\ IF retry_cnt[self] < MaxRetries
                          THEN /\ sgl = 0
                               /\ tsx_mode' = [tsx_mode EXCEPT ![self] = TRUE]
                               /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "L_active_sgl"]
                               /\ UNCHANGED tsx_mode
                    /\ UNCHANGED << mem, sgl, retry_cnt, tx_seq, 
                                    pcl_epoch_start, tsx_buffer, durable_seq, 
                                    crashed, recovered >>

L_active_sgl(self) == /\ pc[self] = "L_active_sgl"
                      /\ \/ /\ sgl = 0
                            /\ tsx_mode' = [tsx_mode EXCEPT ![self] = TRUE]
                            /\ retry_cnt' = [retry_cnt EXCEPT ![self] = 0]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_tsx"]
                            /\ sgl' = sgl
                         \/ /\ sgl = 0 /\ \A other \in Thread \ {self} : ~tsx_mode[other]
                            /\ sgl' = self
                            /\ retry_cnt' = [retry_cnt EXCEPT ![self] = 0]
                            /\ pc' = [pc EXCEPT ![self] = "L_active_sgl_locked"]
                            /\ UNCHANGED tsx_mode
                      /\ UNCHANGED << mem, tx_seq, pcl, pcl_epoch_start, 
                                      tsx_buffer, durable_seq, crashed, 
                                      recovered >>

L_active_sgl_locked(self) == /\ pc[self] = "L_active_sgl_locked"
                             /\ \/ /\ pc' = [pc EXCEPT ![self] = "L_active_sgl_locked"]
                                   /\ UNCHANGED <<mem, sgl, tx_seq, pcl>>
                                \/ /\ \E a \in Addr:
                                        \E v \in Data:
                                          /\ mem' = [mem EXCEPT ![a] = v]
                                          /\ pcl' = [pcl EXCEPT ![self] = Append(pcl[self], <<a, v>>)]
                                   /\ pc' = [pc EXCEPT ![self] = "L_active_sgl_locked"]
                                   /\ UNCHANGED <<sgl, tx_seq>>
                                \/ /\ sgl' = 0
                                   /\ tx_seq' = [tx_seq EXCEPT ![self] = tx_seq[self] + 1]
                                   /\ IF tx_seq'[self] % GroupInterval = 0
                                         THEN /\ pc' = [pc EXCEPT ![self] = "L_group_commit"]
                                         ELSE /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                   /\ UNCHANGED <<mem, pcl>>
                             /\ UNCHANGED << tsx_mode, retry_cnt, 
                                             pcl_epoch_start, tsx_buffer, 
                                             durable_seq, crashed, recovered >>

L_group_commit(self) == /\ pc[self] = "L_group_commit"
                        /\ durable_seq' = [durable_seq EXCEPT ![self] = tx_seq[self]]
                        /\ pcl_epoch_start' = [pcl_epoch_start EXCEPT ![self] = Len(pcl[self]) + 1]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED << mem, sgl, tsx_mode, retry_cnt, tx_seq, 
                                        pcl, tsx_buffer, crashed, recovered >>

ThreadProc(self) == L_idle(self) \/ L_active_tsx(self) \/ L_aborting(self)
                       \/ L_active_sgl(self) \/ L_active_sgl_locked(self)
                       \/ L_group_commit(self)

L_crash == /\ pc[0] = "L_crash"
           /\ \A t \in Thread : pc[t] # "L_group_commit"
           /\ crashed' = [t \in Thread |-> TRUE]
           /\ pc' = [pc EXCEPT ![0] = "Done"]
           /\ UNCHANGED << mem, sgl, tsx_mode, retry_cnt, tx_seq, pcl, 
                           pcl_epoch_start, tsx_buffer, durable_seq, recovered >>

CrashProc == L_crash

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == CrashProc
           \/ (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

TSXSafety ==
    \A t \in Thread : tsx_mode[t] = TRUE => sgl = 0

LockExclusion ==
    \A t1, t2 \in Thread : (sgl = t1 /\ sgl = t2) => t1 = t2

TSXvsSGLSafety ==
    \A t \in Thread : sgl # 0 => ~tsx_mode[t]

DurableSeqMonotonic ==
    \A t \in Thread : durable_seq[t] <= tx_seq[t]

PCLBounds ==
    \A t \in Thread :
        pcl_epoch_start[t] >= 1 /\
        pcl_epoch_start[t] <= Len(pcl[t]) + 1

AtMostOneMode ==
    \A t1, t2 \in Thread :
        ~( tsx_mode[t1] = TRUE /\ pc[t2] = "L_active_sgl_locked" )

TSXBufferInUse ==
    \A t \in Thread :
        (\E a \in Addr : tsx_buffer[t][a] # NoWrite) => tsx_mode[t] = TRUE

(*====================================================================*)
(* Bounding constraints for TLC termination                           *)
(*====================================================================*)
TLCBound ==
    /\ \A t \in Thread : tx_seq[t] < 4
    /\ \A t \in Thread : retry_cnt[t] < MaxRetries + 1
    /\ \A t \in Thread : Len(pcl[t]) < 3

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))
              /\ WF_vars(CrashProc)

ProgressProperty ==
    \A t \in Thread :
        (pc[t] \in {"L_active_tsx", "L_active_sgl", "L_active_sgl_locked"})
        ~> (pc[t] \in {"L_idle", "L_aborting"})

=====================================================================
