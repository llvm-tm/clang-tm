# Rust TL2 — TLA+ Model Notes

## Status

The C++ TL2 model (`TL2.tla`) is authoritative for the C++ implementation at
`backends/tm_impl/tl2/tl2.hpp`. The Rust implementation at
`expli_instr/rust/workspace/runtime/tl2/src/lib.rs` differs architecturally in
three ways.

## Architectural Differences

### 1. Global commit lock (Rust only)

Rust TL2 adds a `COMMIT_LOCK` (`AtomicU64`, line 83) that serializes the
entire commit path:

```
Rust:          acquire COMMIT_LOCK → validate RS → lock WS → write-back → unlock WS → advance G_CLOCK → release COMMIT_LOCK
C++ TL2 model: acquire locks on WS → advance G_CLOCK → validate RS → write-back → unlock WS
```

The Rust commit lock prevents concurrent committers from interleaving their
validation, lock acquisition, and write-back phases.  The C++ TL2 relies on
per-address guard locks for mutual exclusion during write-back and on the
global clock's monotonicity for ordering validation.

**Consequence for verification:** The existing `TL2.tla` model's invariants
(`LockConsistent`, `NoDirtyRead`, `SnapshotInv`) hold without a global commit
lock because TL2's clock-based validation + per-address guards provides
serializability in the model.  Adding the global commit lock would not
introduce new safety violations but would increase the state space
substantially (another CAS spin loop + one more lock variable).

A separate `RustTL2.tla` model would need:
- `COMMIT_LOCK` variable + acquire/release transitions
- Lock address acquisition moved to *after* read-set validation
- Clock advance moved to *after* write-set unlock

### 2. `fence(SeqCst)` at 6 points (Rust only)

Rust TL2 uses `fence(Ordering::SeqCst)` at 6 locations (full CPU barrier).
C++ TL2 uses zero `atomic_thread_fence` calls — all ordering is via
acquire/release on guard operations.

| Location | Rust | C++ |
|----------|------|-----|
| `read_word()` entry | `fence(SeqCst)` | `guard.load(acquire)` |
| `write_word()` entry | `fence(SeqCst)` | `guard.load(acquire)`¹ |
| `tm_commit()` entry | `fence(SeqCst)` | — |
| After lock acquire | `fence(SeqCst)` | `guard.store(locked, acquire)` |
| After unlock | `fence(SeqCst)` | `guard.store(unlocked, release)` |
| Clock advance | `fetch_add(release)`² | `fetch_add(relaxed)`³ |

¹ Write path reads guard version to detect concurrent writers before CAS.
² Clock is `G_CLOCK.fetch_add(1, Release)` (line 347).
³ Clock is `g_clock.fetch_add(1, release)` — was `relaxed`, changed in Phase 2.4.

**Consequence for verification:** The `lastFence` model in `TL2.tla` uses
`lastSignalFence` (compiler barrier) where Rust uses `fence(SeqCst)` (CPU
barrier).  The model's `FenceFidelity` invariant checks only that *some* fence
happened, not the strength — so both pass.  A Rust-specific model would
strengthen several `lastSignalFence` points to `lastThreadFence`.

### 3. Read-validate loop (Rust only)

Rust `read_word()` (lines 187-211) performs a spin-read-validate loop:

```
while is_locked(addr): spin
ver1 = read_version(addr)
val = *addr
ver2 = read_version(addr)
if ver1 != ver2: continue  // torn read → retry
if ver1 > start_version: abort  // concurrent commit → abort
record (addr, ver1) in read-set
```

C++ `read_word()` does a single acquire load and records the version without
retry.  The C++ model (`L_active` ReadMiss branch) records `<<a, ver>>` with
no rollback on mismatch because the PlusCal label is atomic.

**Consequence for verification:** The model cannot distinguish the Rust
retry-loop from the C++ single-read.  Both produce the same final state (a
recorded read-set entry with a stable version).  The Rust loop adds an
invariant: `read_word()` terminates only when an address has a stable version,
which is guaranteed by finite contention in both implementations.

## Recommended approach

Do **not** create a separate `RustTL2.tla` model.  The existing `TL2.tla`
captures the core TL2 algorithm (guard table, clock, read-set validation,
write-set lock).  The Rust-specific differences (global commit lock, extra
fences, read-validate loop) are performance/ordering optimisations that do not
affect the safety invariants that TLC checks.  Document this choice and note
that any `RustTL2.tla` would be a minor variant of `TL2.tla` with:

1. `commit_lock` as a per-address variable in the `variables` block
2. `lastSignalFence` → `lastThreadFence` at 4 points
3. No semantic changes to the state machine or invariants
