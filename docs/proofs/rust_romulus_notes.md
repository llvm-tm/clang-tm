# Rust Romulus — TLA+ Model Notes

## Status

After Phase 4.5 (read-set validation added), the Rust Romulus at
`explicit_api/rust/workspace/runtime/romulus/src/lib.rs` is architecturally
close to the C++ Romulus (`Romulus.tla`).  One structural gap remains:

## Gap: Lock-bit phase (missing in Rust)

C++ Romulus commit path (from `Romulus.tla`):
```
L_validate → L_set_lock_bits → L_inc_clock → L_write_back → L_update_ver → L_release_lock
               ↑                              ↑                ↑
          fetch_or(acq_rel)            thread_fence(sc)  store(release)
          sets bit 0 on version        after write-back  clears bit 0,
          entries, preventing other                       sets version
          threads from reading
```

Rust Romulus commit path:
```
acquire COMMIT_LOCK → validate (WS + RS) → inc clock → write-back → fence → update version → release lock
```

The lock-bit phase (`L_set_lock_bits` + `L_update_ver` clearing it) is absent
in Rust.  Instead, the `COMMIT_LOCK` provides mutual exclusion during
write-back: no other thread can read an inconsistent state because the commit
lock serialises committers.  The `VERSION_TABLE` entries are updated directly
to `commit_ts` (no intermediate lock-bit state).

**Correctness argument:** The lock-bit phase in C++ allows readers to detect
an in-progress write-back (lock bit = 1 → abort).  In Rust, the same
protection comes from `COMMIT_LOCK` — since only one thread can be in the
write-back phase at a time, no reader observes a partially-written version
entry.  Readers that read during a concurrent writer's validate/inc_clock
phase (before write-back) see a consistent version because the version entry
has not yet been updated.

**Fence differences:**

| Phase | C++ (`Romulus.tla`) | Rust |
|-------|---------------------|------|
| Lock acquire | `lastRmw="acquire"` | `compare_exchange(Acquire)` |
| Validate success | `lastSignalFence="sc"` | — |
| Set lock bits | `lastRmw="acq_rel"` | — (no lock-bit phase) |
| Clock increment | `lastRmw="acq_rel"` + `lastThreadFence="sc"` | `fetch_add(AcqRel)` |
| Write-back | `lastThreadFence="sc"` | `fence(SeqCst)` |
| Version update | `lastRmw="release"` | `store(Release)` |
| Release lock | `lastRmw="release"` | `store(Release)` |

Rust adds `fence(SeqCst)` at `read_word()` entry and `write_word()` entry
that have no C++ equivalent.

## Recommended approach

The existing `Romulus.tla` accurately models the core algorithm (version-table
OCC, read-set validation, commit-lock serialised write-back).  The missing
lock-bit phase does not affect the safety invariants that TLC checks
(`LockExclusion`, `ClockMonotonic`, `AtMostOneCommitting`) because the
`COMMIT_LOCK` provides equivalent mutual exclusion.

No separate `RustRomulus.tla` model is needed.  Document this gap and note
that a Rust-specific model would differ only in removing `L_set_lock_bits`
and simplifying `L_update_ver` to a direct `MakeEntry(commit_ts)` store.
