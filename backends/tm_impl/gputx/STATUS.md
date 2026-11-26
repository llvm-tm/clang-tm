# `gputx` backend status

**Status:** `experimental`

GPUTx-style rank/priority concurrency, with commits serialised by one global
commit lock (docs/CORRECTNESS_FIXES.md §35): validate → publish is atomic, which
is what `try_lock_write`'s comment always promised and the code never did.
Correctness gates that cover it: `tools/diff_backends.sh` (including a `-T 16`
leg — the §35 lost update needed ≥16 threads to appear), `test_tx` 120/120,
`test_ds` 207/207, ASan with the leak gate, and UBSan. Deliberately **not** in
`tsan-gate`: reads are plain accesses of memory that committing threads write, so
the backend races in the C++ model (12 data-race reports at `-T 4 -n 200`, all of
them a `do_read()` load against the write-back) until reads get per-slot version
stamps — recorded with the design in `TODO.md`.
