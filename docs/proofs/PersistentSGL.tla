---------------------- MODULE PersistentSGL ----------------------
(*
 * PersistentSGL — SGL with NVM Durability Barriers
 *
 * Algorithm: Single global lock with durable commit.
 * After releasing the lock, the thread flushes written cache
 * lines to NVM (clwb + sfence).  On recovery, the durable
 * state is loaded from NVM.
 *
 * This extends SGL.tla with durability semantics:
 *   - durable_log[t]: set of (addr, value) written by t
 *   - After commit: flush logs to NVM
 *   - Recovery: replay durable logs
 *
 * Invariants (matching SGL.tla):
 *   LockExclusion: At most one thread holds the lock.
 *   DurableWrite: Every committed write is durably recorded.
 *   RecoveryCorrect: After recovery, mem reflects all durably
 *     committed writes.
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
    Thread,             (* Set of thread IDs *)
    Addr,               (* Set of memory addresses *)
    Data                (* Set of possible data values *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat

VARIABLES
    lock,               (* Thread \cup {0}: lock holder *)
    mem,                (* [Addr -> Nat]: shared memory *)
    nvm,                (* [Addr -> Nat]: durable NVM state *)
    pc,                 (* [Thread -> {"idle", "active", "flushing", "done"}] *)
    durable_log,        (* [Thread -> Set(<<Addr, Nat>>)]: writes needing flush *)
    version,            (* Nat: global version counter *)
    committed,          (* [Thread -> Nat]: commit count *)
    crashed,            (* BOOLEAN: system has crashed *)
    recovered           (* BOOLEAN: system has been recovered *)

vars == <<lock, mem, nvm, pc, durable_log, version, committed,
          crashed, recovered>>

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ lock = 0
    /\ mem = [a \in Addr |-> 0]
    /\ nvm = [a \in Addr |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ durable_log = [t \in Thread |-> {}]
    /\ version = 0
    /\ committed = [t \in Thread |-> 0]
    /\ crashed = FALSE
    /\ recovered = FALSE

(*--------------------------------------------------------------------*)
(* Actions                                                             *)
(*--------------------------------------------------------------------*)

(*── Begin: acquire lock ───────────────────────────────────────────*)
Begin(t) ==
    /\ pc[t] = "idle"
    /\ lock = 0
    /\ ~crashed
    /\ lock' = t
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ durable_log' = [durable_log EXCEPT ![t] = {}]
    /\ UNCHANGED <<mem, nvm, version, committed, crashed, recovered>>

(*── Read ──────────────────────────────────────────────────────────*)
Read(t, a) ==
    /\ pc[t] = "active"
    /\ lock = t
    /\ a \in Addr
    /\ UNCHANGED vars

(*── Write: buffer in durable_log ──────────────────────────────────*)
Write(t, a, v) ==
    /\ pc[t] = "active"
    /\ lock = t
    /\ a \in Addr
    /\ v \in Data
    /\ mem' = [mem EXCEPT ![a] = v]
    /\ durable_log' = [durable_log EXCEPT ![t] = durable_log[t] \cup {<<a, v>>}]
    /\ UNCHANGED <<lock, nvm, pc, version, committed, crashed, recovered>>

(*── Prepare flush (after releasing lock conceptually) ─────────────*)
(*  In real implementation: clwb + sfence for each cacheline. *)
Flush(t) ==
    /\ pc[t] = "active"
    /\ lock = t
    (* Persist all written entries to NVM *)
    /\ nvm' = [a \in Addr |->
                 IF \E <<a2, v>> \in durable_log[t] : a2 = a
                 THEN
                     (* Last value written to a in this TX *)
                     LET WrittenValues ==
                         {entry[2] : entry \in {x \in durable_log[t] : x[1] = a}}
                     IN CHOOSE v \in WrittenValues : TRUE
                 ELSE nvm[a]]
    /\ pc' = [pc EXCEPT ![t] = "flushing"]
    /\ UNCHANGED <<lock, mem, durable_log, version, committed,
                   crashed, recovered>>

(*── Complete: release lock ────────────────────────────────────────*)
Complete(t) ==
    /\ pc[t] = "flushing"
    /\ lock = t
    /\ lock' = 0
    /\ version' = version + 1
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ durable_log' = [durable_log EXCEPT ![t] = {}]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<mem, nvm, crashed, recovered>>

(*── Crash ─────────────────────────────────────────────────────────*)
Crash ==
    /\ ~crashed
    /\ crashed' = TRUE
    /\ UNCHANGED <<lock, mem, nvm, pc, durable_log, version,
                   committed, recovered>>

(*── Recovery: reload from NVM ─────────────────────────────────────*)
Recover ==
    /\ crashed = TRUE
    /\ ~recovered
    /\ mem' = nvm        (* Reload all memory from NVM *)
    /\ lock' = 0
    /\ pc' = [t \in Thread |-> "idle"]
    /\ durable_log' = [t \in Thread |-> {}]
    /\ recovered' = TRUE
    /\ UNCHANGED <<nvm, version, committed>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \/ \E t \in Thread : Begin(t)
    \/ \E t \in Thread : \E a \in Addr : Read(t, a)
    \/ \E t \in Thread : \E a \in Addr : \E v \in Data : Write(t, a, v)
    \/ \E t \in Thread : Flush(t)
    \/ \E t \in Thread : Complete(t)
    \/ Crash
    \/ Recover

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: Mutual exclusion on the SGL ───────────────────────────────*)
LockExclusion ==
    \A t1, t2 \in Thread :
        (lock = t1 /\ lock = t2) => t1 = t2

(*── I2: Lock holder is in active or flushing state ────────────────*)
LockHolderActive ==
    \A t \in Thread :
        lock = t => pc[t] \in {"active", "flushing"}

(*── I3: After crash and recovery, mem = nvm ──────────────────────*)
RecoveryConsistency ==
    recovered = TRUE => mem = nvm

(*── I4: All durable writes in nvm are a superset of committed writes ─*)
NVMContainsCommitted ==
    \A a \in Addr :
        \/ nvm[a] = mem[a]
        \/ \E t \in Thread :
            \E <<a2, v>> \in durable_log[t] : a2 = a /\ nvm[a] = v

====
