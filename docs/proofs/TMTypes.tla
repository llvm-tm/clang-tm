----------------------------- MODULE TMTypes -----------------------------
(*
 * Shared types and operators for TM backend TLA+ models.
 *
 * Fenced(t, signal, thread, rmw):
 *   Checks if any fence variable is set for thread t.
 *   Used in FenceFidelity invariants across all backends.
 *
 * NoWrite:
 *   Sentinel value for per-address write-set arrays.
 *   Indicates no thread has written to that address.
 *)
EXTENDS Naturals

Fenced(t, signal, thread, rmw) ==
    signal[t] # "" \/ thread[t] # "" \/ rmw[t] # ""

NoWrite == 0 - 1

=======================================================================
