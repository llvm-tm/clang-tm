# Implementation Notes: JVSTM (multi-version OCC)

Based on the Java Versioned Software Transactional Memory
(Cachopo & Rito-Silva, 2006): versioned boxes (VBoxes) holding a
newest-first list of `(version, value)` bodies, snapshot reads at
`begin`, commit-time validation under a global commit lock.

## Known limitation: `.peek()` and direct memory reads

**Status: known, documented, unresolved by design.**

`test_tx` reports 25 failures with this backend. Root cause: the test's
`.peek()`-style accessors read the *original memory address* directly,
but JVSTM never writes committed values back in place — each commit
prepends a new body to the address's VBox history, and readers walking
the history select the newest body with `version <= read_version`.
The authoritative value therefore lives in the version list, not at the
original address, so any non-TM read of that address sees stale data.

This is not a concurrency bug (no torn state, no lost update); it is an
API-semantics mismatch inherent to multi-version designs that do not
write through: **"read the newest committed value of address x" is only
well-defined through the TM API**, because "newest" depends on the
reader's snapshot policy. In-place designs (TL2, NOrec, SGL) can expose
the same value through a plain load; write-behind MVCC designs cannot.

Consequences for tests and benchmarks:

- `test_ds`: 207/207 PASS (all accesses go through the TM API).
- `test_tx`: 25 failures, all via direct/peek reads.
- Do not teach `.peek()` semantics as backend-portable; treat direct
  reads as valid only for backends that write through to the address.
