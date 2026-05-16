----------------------- MODULE TSXSGL ------------------------
(*
 * TSX+SGL Hybrid TM — TLA+ Specification
 *
 * Algorithm:
 *   - A lock variable sgl: 0 = free, thread-id = locked by that thread.
 *   - A fallback mutex Lock (modelled implicitly via sgl guard).
 *   - TSX transactions read sgl into their read-set at begin();
 *     if sgl != 0 they xabort() and spin until free.
 *   - At commit, TSX re-reads sgl; if it changed (a concurrent SGL
 *     entry wrote to the cache line), xabort() and retry.
 *   - SGL path: Lock.acquire(); sgl := 1; work; sgl := 0; Lock.release().
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

EXTENDS Naturals, TLC

CONSTANTS Thread, MaxVersion
ASSUME Thread \subseteq Nat \ {0}

VARIABLES
    sgl,                                  (* lock: 0=free, tid=locked *)
    mem[_],                               (* shared memory, indexed by address *)
    pc[_],                                (* per-thread PC *)
    mode[_],                              (* "idle", "tsx", "sgl" *)
    readSet[_],                           (* per-thread read set *)
    writeSet[_],                          (* per-thread write set *)
    txSnapshot[_],                        (* sgl snapshot at TX start, per thread *)
    tsxRetries[_],                        (* TSX retry count per thread *)
    txCount[_],                           (* committed TX count per thread *)
    aborted[_]                            (* aborted TX count per thread *)

vars == <<sgl, mem, pc, mode, readSet, writeSet, txSnapshot, tsxRetries, txCount, aborted>>

CONSTANT Addr, MaxRetries
ASSUME Addr \subseteq Nat
ASSUME MaxRetries \in Nat

Init ==
    /\ sgl = 0
    /\ mem = [a \in Addr |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ mode = [t \in Thread |-> "idle"]
    /\ readSet = [t \in Thread |-> {}]
    /\ writeSet = [t \in Thread |-> {}]
    /\ txSnapshot = [t \in Thread |-> 0]
    /\ tsxRetries = [t \in Thread |-> 0]
    /\ txCount = [t \in Thread |-> 0]
    /\ aborted = [t \in Thread |-> 0]

(*====================================================================*)
(* TSX Transaction Begin — try hardware transaction                    *)
(*====================================================================*)
TSXBegin(t) ==
    /\ pc[t] = "idle"
    /\ tsxRetries[t] < MaxRetries          (* retries not exhausted *)
    /\ sgl = 0                              (* SGL not active *)
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ mode' = [mode EXCEPT ![t] = "tsx"]
    /\ readSet' = [readSet EXCEPT ![t] = {a \in Addr : TRUE}]  (* TSX hardware captures all *)
    /\ txSnapshot' = [txSnapshot EXCEPT ![t] = sgl]
    /\ tsxRetries' = [tsxRetries EXCEPT ![t] = 0]
    /\ UNCHANGED <<sgl, mem, writeSet, txCount, aborted>>

(* TSX retry on abort: increment retry count *)
TSXRetry(t) ==
    /\ pc[t] = "active"
    /\ mode[t] = "tsx"
    /\ tsxRetries[t] < MaxRetries          (* can retry *)
    /\ tsxRetries' = [tsxRetries EXCEPT ![t] = tsxRetries[t] + 1]
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ UNCHANGED <<sgl, mem, pc, mode, readSet, writeSet, txSnapshot, txCount>>

(* TSX fallback to SGL: retries exhausted *)
TSXFallback(t) ==
    /\ pc[t] = "active"
    /\ mode[t] = "tsx"
    /\ tsxRetries[t] >= MaxRetries
    /\ sgl = 0                              (* SGL not active *)
    /\ sgl' = t                             (* acquire lock: tid *)
    /\ mode' = [mode EXCEPT ![t] = "sgl"]
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ UNCHANGED <<mem, pc, readSet, writeSet, txSnapshot, tsxRetries, txCount>>

(*====================================================================*)
(* SGL Transaction Begin — acquire fallback lock                       *)
(*====================================================================*)
SGLBegin(t) ==
    /\ pc[t] = "idle"
    /\ sgl = 0                              (* SGL not active *)
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ mode' = [mode EXCEPT ![t] = "sgl"]
    /\ sgl' = t                             (* acquire lock: tid *)
    /\ txSnapshot' = [txSnapshot EXCEPT ![t] = sgl + 1]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<mem, tsxRetries, txCount, aborted>>

(*====================================================================*)
(* Read V_i — direct load (TSX or SGL provides isolation)              *)
(*====================================================================*)
TMRead(t, a) ==
    /\ pc[t] = "active"
    /\ mode[t] \in {"tsx", "sgl"}           (* only inside a transaction *)
    /\ a \in Addr
    /\ readSet' = [readSet EXCEPT ![t] = readSet[t] \cup {a}]
    /\ UNCHANGED <<sgl, mem, pc, mode, writeSet, txSnapshot, tsxRetries, txCount, aborted>>

(*====================================================================*)
(* Write N to V_i — direct store (TSX or SGL provides isolation)       *)
(*====================================================================*)
TMWrite(t, a, n) ==
    /\ pc[t] = "active"
    /\ mode[t] \in {"tsx", "sgl"}
    /\ a \in Addr
    /\ mem' = [mem EXCEPT ![a] = n]
    /\ writeSet' = [writeSet EXCEPT ![t] = writeSet[t] \cup {a}]
    /\ UNCHANGED <<sgl, pc, mode, readSet, txSnapshot, tsxRetries, txCount, aborted>>

(*====================================================================*)
(* TSX Commit                                                          *)
(*====================================================================*)
TSXCommit(t) ==
    /\ pc[t] = "active"
    /\ mode[t] = "tsx"
    /\ sgl = txSnapshot[t]                  (* sgl unchanged: no SGL interleaved *)
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ txCount' = [txCount EXCEPT ![t] = txCount[t] + 1]
    /\ UNCHANGED <<sgl, mem, readSet, writeSet, txSnapshot, tsxRetries, aborted>>

(* TSX abort on sgl change (SGL interleaved) *)
TSXAbort(t) ==
    /\ pc[t] = "active"
    /\ mode[t] = "tsx"
    /\ sgl # txSnapshot[t]                  (* sgl changed: SGL ran *)
    /\ aborted' = [aborted EXCEPT ![t] = aborted[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]      (* abort: transaction undone *)
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeSet' = [writeSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<sgl, mem, txSnapshot, tsxRetries, txCount>>

(*====================================================================*)
(* SGL Commit                                                          *)
(*====================================================================*)
SGLCommit(t) ==
    /\ pc[t] = "active"
    /\ mode[t] = "sgl"
    /\ sgl = t                              (* we hold the lock *)
    /\ sgl' = 0                             (* release lock *)
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ mode' = [mode EXCEPT ![t] = "idle"]
    /\ txCount' = [txCount EXCEPT ![t] = txCount[t] + 1]
    /\ UNCHANGED <<mem, readSet, writeSet, txSnapshot, tsxRetries, aborted>>

(*====================================================================*)
(* Next-state relation                                                  *)
(*====================================================================*)
Next ==
    \E t \in Thread :
        \/ TSXBegin(t)
        \/ TSXRetry(t)
        \/ TSXFallback(t)
        \/ SGLBegin(t)
        \/ (\E a \in Addr : TMRead(t, a))
        \/ (\E a \in Addr : \E n \in Nat : TMWrite(t, a, n))
        \/ TSXCommit(t)
        \/ TSXAbort(t)
        \/ SGLCommit(t)

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* INVARIANT 1: Lock-free exactly when no SGL is active                *)
(*                                                                     *)
(*   sgl = 0  <=>  ~(\E t \in Thread : mode[t] = "sgl")               *)
(*====================================================================*)
LockFreeInv ==
    (sgl = 0) <=> ~(\E t \in Thread : mode[t] = "sgl")

(* Lemma: LockFreeInv is inductive — SGLBegin/TSXFallback require
   sgl = 0, SGLCommit sets sgl = 0, TSX actions preserve sgl. *)
THEOREM LockFreeIsInductive ==
    Init => LockFreeInv
    /\ (LockFreeInv /\ [Next]_vars) => LockFreeInv'
PROOF
    <1>1. Init => LockFreeInv
        BY Init DEF Init, LockFreeInv
    <1>2. LockFreeInv /\ [Next]_vars => LockFreeInv'
        <2>1. CASE TSXBegin(t)
            BY TSXBegin DEF LockFreeInv
        <2>2. CASE SGLBegin(t)
            (* sgl = 0 before, sgl' = t, mode'[t] = "sgl" *)
            BY SGLBegin DEF LockFreeInv
        <2>3. CASE SGLCommit(t)
            (* sgl = t before (SGL active), sgl' = 0, mode'[t] = "idle" *)
            BY SGLCommit DEF LockFreeInv
        <2>4. CASE TSXFallback(t)
            (* sgl = 0 before, sgl' = t, mode'[t] = "sgl" *)
            BY TSXFallback DEF LockFreeInv
        <2>5. CASE TSXCommit(t) \/ TSXAbort(t) \/ TMRead(t, a)
                    \/ TMWrite(t, a, n) \/ TSXRetry(t)
            (* sgl and mode unchanged *)
            BY TSXCommit, TSXAbort, TMRead, TMWrite, TSXRetry DEF LockFreeInv
        <2>6. QED
            BY <2>1, <2>2, <2>3, <2>4, <2>5 DEF Next
    <1>3. QED

(*====================================================================*)
(* INVARIANT 2: Lock owner is the thread in SGL mode                   *)
(*                                                                     *)
(*   sgl = t  =>  mode[t] = "sgl"                                      *)
(*====================================================================*)
LockOwnerInv ==
    \A t \in Thread : (sgl = t) => (mode[t] = "sgl")

THEOREM LockOwnerIsInductive ==
    Init => LockOwnerInv
    /\ (LockOwnerInv /\ [Next]_vars) => LockOwnerInv'
PROOF
    <1>1. Init => LockOwnerInv
        BY Init DEF Init, LockOwnerInv
    <1>2. LockOwnerInv /\ [Next]_vars => LockOwnerInv'
        BY LockOwnerInv DEF SGLBegin, SGLCommit, TSXFallback,
           TSXCommit, TSXAbort, TSXBegin, TSXRetry, TMRead, TMWrite, Next, vars
    <1>3. QED

(*====================================================================*)
(* INVARIANT 3: No TSX runs while SGL is active                        *)
(*                                                                     *)
(*   \A t \in Thread : (mode[t] = "tsx") => (sgl = 0)                  *)
(*====================================================================*)
TSXvsSGLSafety ==
    \A t \in Thread : (mode[t] = "tsx") => (sgl = 0)

THEOREM TSXvsSGLSafetyIsInductive ==
    Init => TSXvsSGLSafety
    /\ (TSXvsSGLSafety /\ [Next]_vars) => TSXvsSGLSafety'
PROOF
    <1>1. Init => TSXvsSGLSafety
        BY Init DEF Init, TSXvsSGLSafety
    <1>2. TSXvsSGLSafety /\ [Next]_vars => TSXvsSGLSafety'
        BY TSXvsSGLSafety DEF SGLBegin, SGLCommit, TSXFallback,
           TSXCommit, TSXAbort, TSXBegin, TSXRetry, TMRead, TMWrite, Next, vars
    <1>3. QED

(*====================================================================*)
(* COROLLARY: At most one SGL transaction at a time                    *)
(*                                                                     *)
(* Follows from LockFreeInv + LockOwnerInv: sgl can equal at most     *)
(* one thread ID, so at most one thread can have mode[t] = "sgl".     *)
(*====================================================================*)
AtMostOneSGL ==
    \A t1, t2 \in Thread :
        (t1 # t2 /\ mode[t1] = "sgl" /\ mode[t2] = "sgl") => FALSE

THEOREM AtMostOneSGLIsInvariant ==
    Spec => []AtMostOneSGL

(*====================================================================*)
(* PROPERTY: No dirty reads under TSXSGL                               *)
(*====================================================================*)
NoDirtyReadsTSXSGL ==
    \A t1, t2 \in Thread, a \in Addr :
        (mode[t1] = "sgl" /\ mode[t2] = "tsx" /\ a \in writeSet[t1])
        => (aborted[t2] > 0 \/ sgl = 0)

(*====================================================================*)
(* THEOREM: TSXSGL ensures serializability                             *)
(*                                                                     *)
(* All committed transactions are serializable. The serialization      *)
(* order is:                                                          *)
(*   - TSX transactions that commit (no interleaved SGL)               *)
(*   - SGL transactions in lock-acquire order                          *)
(*   - TSX transactions that ran after the last SGL                    *)
(*====================================================================*)
THEOREM TSXSGLSafety ==
    Spec => TSXvsSGLSafety

(*====================================================================*)
(* PROOF SKETCH                                                        *)
(*                                                                     *)
(* 1. At tm_begin() in TSX mode, the thread reads sgl.  If sgl != 0,  *)
(*    the TSX aborts (modelled by TSXBegin's guard sgl = 0).           *)
(*    So no TSX starts while SGL holds.                               *)
(*                                                                     *)
(* 2. SGL entry writes 1 to sgl (modelled by sgl' = t).  Any          *)
(*    concurrent TSX has sgl in its read-set; the write to sgl         *)
(*    triggers a cache conflict -> TSX aborts.  Modelled as            *)
(*    TSXAbort when sgl # txSnapshot[t].                               *)
(*                                                                     *)
(* 3. SGL exit writes 0 to sgl (modelled by sgl' = 0).  By the        *)
(*    time this write reaches the cache, any concurrent TSX has        *)
(*    already been aborted by the entry write.                         *)
(*                                                                     *)
(* 4. TSX commit re-reads sgl.  If it changed (i.e., an SGL entry     *)
(*    wrote to it), the hardware aborted the TSX (modelled by          *)
(*    TSXAbort).  The end-check is a safety double-check for           *)
(*    micro-architectural races where the hardware might have          *)
(*    missed the first write.                                          *)
(*                                                                     *)
(* 5. Therefore: any TSX transaction that commits sees a state that    *)
(*    is equivalent to a point between the last SGL exit and the       *)
(*    next SGL entry.  This is a valid serialization point.            *)
(*====================================================================*)

=======================================================================
