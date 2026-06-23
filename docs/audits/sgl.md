# Audit: Single Global Lock (SGL)

**Score: 5/5** — Perfect match. TLA+ model faithfully represents the C++ implementation.

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/sgl.tla` (PlusCal + TLAPS, 346 lines) |
| C++ runtime | `backends/tm_impl/single_global_lock/SingleGlobalLock_runtime.cpp` (257 lines) |
| TLC config | `docs/proofs/sgl.cfg` |

## Algorithm Summary

All transactions acquire a single global mutex at `begin()`, read/write memory directly while holding it, release at `end()`. Serial isolation ensures correctness. No read-set, write-set, or version tracking in the C++ runtime.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| `global_tx_lock.lock()` in `real_tm_begin` | L_idle: await lock=0; lock := self | ✅ | |
| `global_tx_lock.unlock()` in `real_tm_end` | Commit: lock := 0 | ✅ | |
| `*addr` read in `real_tm_read_i4` etc. | L_active: Read with Addr union | ✅ | Direct memory access |
| `*addr = val` write in `real_tm_write_i4` etc. | L_active: Write with mem update | ✅ | Direct memory access |

## Invariants

| Invariant | C++ check | TLC result |
|-----------|-----------|------------|
| MutexInv (mutual exclusion on lock) | `std::mutex` guarantees | ✅ PASS |
| AtMostOneActive | Only one thread holds lock at a time | ✅ PASS |
| NoDirtyReads | Exclusive access prevents dirty reads | ✅ PASS |

## Deviations

### 1. Read-set/write-set/version scaffolding (No risk)
**TLA+** tracks `readSet`, `writeSet`, `version`, `readVersion`, `aborted`, `committed` for proof purposes.
**C++** has none of these — the global lock provides serial isolation.

The TLA+ spec's own header comment documents this: "The spec's read-set/write-set/version variables are proof scaffolding and do not correspond to runtime state."

**Risk**: None — documented in the spec.

### 2. Version bound (No risk)
**TLA+** (`sgl.cfg`): `VersionBound == version < 3` limits the clock for bounded model checking.
**C++**: No version tracking.

**Risk**: None — `VersionBound` does not appear in any invariant.

## Summary

| Aspect | Verdict |
|--------|---------|
| Core protocol (lock/unlock) | ✅ Perfect match |
| Invariants | ✅ All invariants hold in C++ by construction |
| Known deviations | 2 deviations (both documented scaffolding) |
| **Overall score** | **5/5** |
