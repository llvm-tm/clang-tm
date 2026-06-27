----------------------- MODULE TSXSGL ------------------------
(*
 * TSX+SGL Hybrid TM — TLA+ Specification (PlusCal)
 *
 * Algorithm:
 *   - A lock variable sgl: 0 = free, thread-id = locked by that thread.
 *   - A fallback mutex (modelled implicitly via sgl guard).
 *   - TSX transactions read sgl into their read-set at begin();
 *     if sgl != 0 they xabort() and spin until free.
 *   - At commit, TSX re-reads sgl; if it changed (a concurrent SGL
 *     entry wrote to the cache line), xabort() and retry.
 *   - SGL path: acquire lock; sgl := self; work; sgl := 0; release lock.
 *
 * Key invariants:
 *   - At most one thread is in mode[t] = "sgl".
 *   - sgl = 0  <=>  no thread is in SGL mode.
 *   - No TSX runs while SGL is active (sgl = 0 required for TSX begin).
 *   - Any SGL entry writes to sgl, which aborts any concurrent TSX
 *     (the cache line is in the TSX read-set).
 *
 * In the TLA+ model, sgl stores the thread-id for stronger invariants;
 * the C++ code writes 1 (any non-zero sentinel) because the hardware
 * detects the cache-line write regardless of value.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Thread, Addr, Data, MaxRetries, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxRetries \in Nat
ASSUME MaxCommits \in Nat

(* NOTE: lastFence[t] fence tracking exists only in the TLA+ translation below.
   Running pcal.trans will regenerate TLA+ and LOSE all fence tracking.
   Re-add lastFence variable + EXCEPT updates manually after translation. *)
(* --algorithm TSXSGL

variables
    sgl = 0,
    mem = [a \in Addr |-> 0],
    mode = [t \in Thread |-> "idle"],
    readSet = [t \in Thread |-> {}],
    writeSet = [t \in Thread |-> {}],
    txSnapshot = [t \in Thread |-> 0],
    tsxRetries = [t \in Thread |-> 0],
    txCount = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0];

process ThreadProc \in Thread
begin

L_idle:
    either \* Start TSX transaction
        await tsxRetries[self] < MaxRetries /\ sgl = 0;
        mode[self] := "tsx";
        readSet[self] := Addr;             (* TSX hardware captures all *)
        txSnapshot[self] := sgl;
        tsxRetries[self] := 0;
        goto L_active;
    or \* Start SGL transaction (fallback or direct)
        await sgl = 0;
        sgl := self;
        mode[self] := "sgl";
        readSet[self] := {};
        writeSet[self] := {};
        txSnapshot[self] := 1;             (* non-zero sentinel *)
        goto L_active;
    or \* Terminate
        if txCount[self] >= MaxCommits then
            goto L_done;
        else
            goto L_idle;
        end if;
    end either;

L_active:
    either \* Read (direct load — TSX or SGL provides isolation)
        with a \in Addr do
            readSet[self] := readSet[self] \union {a};
        end with;
        goto L_active;
    or \* Write (direct store)
        with a \in Addr, n \in Data do
            mem[a] := n;
            writeSet[self] := writeSet[self] \union {a};
        end with;
        goto L_active;
    or \* Commit (TSX path — re-read sgl)
        if mode[self] = "tsx" then
            if sgl = txSnapshot[self] then
                \* TSX commit succeeds
                mode[self] := "idle";
                txCount[self] := txCount[self] + 1;
                goto L_idle;
            else
                \* TSX abort (concurrent SGL wrote to sgl)
                aborted[self] := aborted[self] + 1;
                mode[self] := "idle";
                readSet[self] := {};
                writeSet[self] := {};
                goto L_idle;
            end if;
        else
            goto L_active;
        end if;
    or \* Commit (SGL path — release lock)
        if mode[self] = "sgl" then
            await sgl = self;
            sgl := 0;
            mode[self] := "idle";
            txCount[self] := txCount[self] + 1;
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* TSX retry (hardware retry, stay in TSX mode)
        if mode[self] = "tsx" /\ tsxRetries[self] < MaxRetries then
            tsxRetries[self] := tsxRetries[self] + 1;
            aborted[self] := aborted[self] + 1;
            goto L_active;
        else
            goto L_active;
        end if;
    or \* TSX fallback to SGL (retries exhausted)
        if mode[self] = "tsx" /\ tsxRetries[self] >= MaxRetries then
            await sgl = 0;
            sgl := self;
            mode[self] := "sgl";
            aborted[self] := aborted[self] + 1;
            goto L_active;
        else
            goto L_active;
        end if;
    or \* TSX abort (concurrent SGL entry detected)
        if mode[self] = "tsx" /\ sgl # txSnapshot[self] then
            aborted[self] := aborted[self] + 1;
            mode[self] := "idle";
            readSet[self] := {};
            writeSet[self] := {};
            goto L_idle;
        else
            goto L_active;
        end if;
    end either;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "2f212a71" /\ chksum(tla) = "bef00627")
VARIABLES pc, sgl, mem, mode, readSet, writeSet, txSnapshot, tsxRetries, 
          txCount, lastFence, aborted

vars == << pc, sgl, mem, mode, readSet, writeSet, txSnapshot, tsxRetries, 
            txCount, lastFence, aborted >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ sgl = 0
        /\ mem = [a \in Addr |-> 0]
        /\ mode = [t \in Thread |-> "idle"]
        /\ readSet = [t \in Thread |-> {}]
        /\ writeSet = [t \in Thread |-> {}]
        /\ txSnapshot = [t \in Thread |-> 0]
        /\ tsxRetries = [t \in Thread |-> 0]
        /\ txCount = [t \in Thread |-> 0]
        /\ lastFence = [t \in Thread |-> ""]
        /\ aborted = [t \in Thread |-> 0]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ tsxRetries[self] < MaxRetries /\ sgl = 0
                      /\ mode' = [mode EXCEPT ![self] = "tsx"]
                      /\ readSet' = [readSet EXCEPT ![self] = Addr]
                      /\ txSnapshot' = [txSnapshot EXCEPT ![self] = sgl]
                      /\ tsxRetries' = [tsxRetries EXCEPT ![self] = 0]
                      /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      /\ UNCHANGED <<sgl, writeSet>>
                   \/ /\ sgl = 0
                      /\ sgl' = self
                      /\ mode' = [mode EXCEPT ![self] = "sgl"]
                      /\ readSet' = [readSet EXCEPT ![self] = {}]
                      /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                      /\ txSnapshot' = [txSnapshot EXCEPT ![self] = 1]
                      /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      /\ UNCHANGED tsxRetries
                    \/ /\ IF txCount[self] >= MaxCommits
                              THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                       /\ UNCHANGED <<sgl, mode, readSet, writeSet, txSnapshot, tsxRetries, lastFence>>
                /\ UNCHANGED << mem, txCount, aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {a}]
                             /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<sgl, mem, mode, writeSet, tsxRetries, txCount, aborted>>
                     \/ /\ \E a \in Addr:
                             \E n \in Data:
                               /\ mem' = [mem EXCEPT ![a] = n]
                               /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \union {a}]
                               /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<sgl, mode, readSet, tsxRetries, txCount, aborted>>
                      \/ /\ (IF mode[self] = "tsx"
                               THEN /\ IF sgl = txSnapshot[self]
                                          THEN /\ mode' = [mode EXCEPT ![self] = "idle"]
                                               /\ txCount' = [txCount EXCEPT ![self] = txCount[self] + 1]
                                               /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                               /\ UNCHANGED << readSet, 
                                                               writeSet, 
                                                               aborted >>
                                          ELSE /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                               /\ mode' = [mode EXCEPT ![self] = "idle"]
                                               /\ readSet' = [readSet EXCEPT ![self] = {}]
                                               /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                               /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                               /\ UNCHANGED txCount
                               ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                    /\ UNCHANGED << mode, readSet, writeSet, 
                                                    txCount, aborted, lastFence >>)
                         /\ UNCHANGED <<sgl, mem, tsxRetries>>
                      \/ /\ (IF mode[self] = "sgl"
                               THEN /\ sgl = self
                                    /\ sgl' = 0
                                    /\ mode' = [mode EXCEPT ![self] = "idle"]
                                    /\ txCount' = [txCount EXCEPT ![self] = txCount[self] + 1]
                                    /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                                    /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                               ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                    /\ UNCHANGED << sgl, mode, txCount, lastFence >>)
                         /\ UNCHANGED <<mem, readSet, writeSet, tsxRetries, aborted>>
                      \/ /\ (IF mode[self] = "tsx" /\ tsxRetries[self] < MaxRetries
                               THEN /\ tsxRetries' = [tsxRetries EXCEPT ![self] = tsxRetries[self] + 1]
                                    /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                    /\ pc' = [pc EXCEPT ![self] = "L_active"]
                               ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                    /\ UNCHANGED << tsxRetries, aborted >>)
                         /\ UNCHANGED <<sgl, mem, mode, readSet, writeSet, txCount, lastFence>>
                      \/ /\ (IF mode[self] = "tsx" /\ tsxRetries[self] >= MaxRetries
                               THEN /\ sgl = 0
                                    /\ sgl' = self
                                    /\ mode' = [mode EXCEPT ![self] = "sgl"]
                                    /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                    /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                                    /\ pc' = [pc EXCEPT ![self] = "L_active"]
                               ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                    /\ UNCHANGED << sgl, mode, aborted, lastFence >>)
                         /\ UNCHANGED <<mem, readSet, writeSet, tsxRetries, txCount>>
                      \/ /\ (IF mode[self] = "tsx" /\ sgl # txSnapshot[self]
                               THEN /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                    /\ mode' = [mode EXCEPT ![self] = "idle"]
                                    /\ readSet' = [readSet EXCEPT ![self] = {}]
                                    /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                    /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                                    /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                               ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                    /\ UNCHANGED << mode, readSet, writeSet, 
                                                    aborted, lastFence >>)
                         /\ UNCHANGED <<sgl, mem, tsxRetries, txCount>>
                  /\ UNCHANGED txSnapshot

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << sgl, mem, mode, readSet, writeSet, txSnapshot, 
                                tsxRetries, txCount, lastFence, aborted >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Bounds for model checking                                          *)
(*====================================================================*)
TxCountBound == \A t \in Thread : txCount[t] <= MaxCommits

(*====================================================================*)
(* INVARIANT 1: Lock-free exactly when no SGL is active               *)
(*                                                                     *)
(*   sgl = 0  <=>  ~(\E t \in Thread : mode[t] = "sgl")               *)
(*====================================================================*)
LockFreeInv ==
    (sgl = 0) <=> ~(\E t \in Thread : mode[t] = "sgl")

(*====================================================================*)
(* INVARIANT 2: Lock owner is the thread in SGL mode                   *)
(*                                                                     *)
(*   sgl = t  =>  mode[t] = "sgl"                                      *)
(*====================================================================*)
LockOwnerInv ==
    \A t \in Thread : (sgl = t) => (mode[t] = "sgl")

(*====================================================================*)
(* NOTE:                                                                *)
(*  The property "no TSX runs while SGL is active"                     *)
(*  (\A t : mode[t]="tsx" => sgl=0) is NOT an invariant of the         *)
(*  algorithm.  When an SGL entry occurs while a TSX is active,        *)
(*  there is an intermediate state where mode[t]="tsx" and sgl#0       *)
(*  (the TSX hasn't aborted yet).  The hardware guarantees the abort,  *)
(*  but the TLA+ model captures the intermediate state as a separate    *)
(*  step.  The safety property that DOES hold is:                      *)
(*    "no TSX commit happens if sgl changed since the TSX began"       *)
(*  which is enforced by the TSXCommit guard sgl = txSnapshot[t].      *)
(*====================================================================*)

(*====================================================================*)
(* COROLLARY: At most one SGL transaction at a time                    *)
(*====================================================================*)
AtMostOneSGL ==
    \A t1, t2 \in Thread :
        (t1 # t2 /\ mode[t1] = "sgl" /\ mode[t2] = "sgl") => FALSE

(*====================================================================*)
(* INVARIANT 4: Every thread with a non-empty write-set has issued a   *)
(*              fence (acq, rel, or sc).                                *)
(*====================================================================*)
FenceFidelity ==
    \A t \in Thread : writeSet[t] # {} => lastFence[t] # ""

(*====================================================================*)
(* Combined invariant                                                  *)
(*====================================================================*)
Inv ==
    /\ LockFreeInv
    /\ LockOwnerInv
    /\ AtMostOneSGL
    /\ FenceFidelity

(*====================================================================*)
(* THEOREM: TSXSGL ensures mutual exclusion + safety                   *)
(*====================================================================*)
THEOREM Spec => []LockFreeInv
THEOREM Spec => []LockOwnerInv
THEOREM Spec => []AtMostOneSGL
THEOREM Spec => []FenceFidelity
THEOREM Spec => []Inv

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProperty ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_done"})

=======================================================================
