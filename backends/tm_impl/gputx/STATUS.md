# `gputx` backend status

**Status:** `experimental`

GPUTx-style rank/priority concurrency, with two things the original scaffold
lacked: commits are serialised by one global commit lock, so validate → publish is
atomic (docs/CORRECTNESS_FIXES.md §35), and its lock-free reads are width-exact
relaxed atomics paired with per-word commit counters, so validation does not have to
trust a value alone and the accesses are not undefined behaviour (§37). That makes
the backend race-clean under ThreadSanitizer, so it is gated like the rest.

Correctness gates covering it: `tsan-gate`, `ubsan-gate`, ASan including the leak
gate, the `tests/backends/tm_impl` unit suite, and `tools/diff_backends.sh` (with a
`-T 16` leg — the §35 lost update needed ≥16 threads to appear).

Kept at `experimental` deliberately, despite the full gate coverage:

- the rank scheme never displaces a lock holder — it spins, then aborts, relying on
  age escalation for fairness, and under ThreadSanitizer that path does not finish
  at 16 threads (`TODO.md`);
- the stronger validation costs aborts, 24–29 % more at identical commit totals
  (§37's table), and the 1 M-entry version table can alias two far-apart addresses
  onto one counter, which is a spurious abort, never a wrong commit;
- none of this has been measured against the other backends on a contention-heavy
  benchmark. Promotion needs that comparison, not just green CI.
