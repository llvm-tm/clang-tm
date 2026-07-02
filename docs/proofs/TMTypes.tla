----------------------------- MODULE TMTypes -----------------------------
(*
 * Shared types and operators for TM backend TLA+ models.
 *
 * Fenced(t, signal, thread, rmw):
 *   Checks if any fence variable is set for thread t.
 *
 * FenceFidelity(Thread, writeSet, signal, thread, rmw):
 *   Every thread holding locks has done at least one fence.
 *
 * MutexInv(lock, Thread):
 *   At most one thread can hold the given lock variable.
 *
 * LockOwnerInv(lock, Thread, states, allowed):
 *   If lock is held by thread t, then t's state is in the allowed set.
 *
 * ProgressProperty(Thread, progressState, targetStates):
 *   Every thread in progressState eventually reaches a target state.
 *
 * LockedVersion — arithmetic encoding for TL2/Romulus family:
 *   LockBit(e) == e % 2           bit 0 = lock flag
 *   VersionOf(e) == e \div 2      remaining bits = version
 *   MakeEntry(v) == v * 2         version + unlocked
 *
 * NoWrite:
 *   Sentinel for per-address write-set arrays.
 *)
EXTENDS Naturals

\* ── Fence tracking ──────────────────────────────────────────
Fenced(t, signal, thread, rmw) ==
    signal[t] # "" \/ thread[t] # "" \/ rmw[t] # ""

FenceFidelity(Thread, writeSet, signal, threadFence, rmw) ==
    \A t \in Thread :
        (writeSet[t] # {}) => Fenced(t, signal, threadFence, rmw)

FenceFidelityPA(Thread, writeSet, signal, threadFence, rmw) ==
    \A t \in Thread :
        (\E a \in DOMAIN writeSet[t] : writeSet[t][a] # NoWrite) =>
            Fenced(t, signal, threadFence, rmw)

\* ── Lock invariants ─────────────────────────────────────────
MutexInv(lock, Thread) ==
    \A t1, t2 \in Thread : (lock = t1 /\ lock = t2) => t1 = t2

LockOwnerInv(lock, Thread, state, allowedStates) ==
    \A t \in Thread : (lock = t) => state[t] \in allowedStates

\* ── Liveness ────────────────────────────────────────────────
ProgressProperty(Thread, label, progressState, targetStates) ==
    \A self \in Thread :
        (pc[self] = progressState) ~> (pc[self] \in targetStates)

\* ── Version-lock encoding (TL2, Romulus, LEFTRIGHT) ─────────
LockBit(e) == e % 2
VersionOf(e) == e \div 2
MakeEntry(v) == v * 2

\* ── Sentinel ────────────────────────────────────────────────
NoWrite == 0 - 1

=======================================================================
