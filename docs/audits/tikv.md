# TiKV Distributed TM Backend — Audit

**Score: 2/5** — Async runtime (`tokio`, `block_on`), gRPC error handling, at-most-once delivery semantics not captured. No `lastFence` tracking. Unbounded counters prevent TLC termination. Model captures Percolator 2PC at high level but misses all distributed-systems detail. **Downgraded from memory ordering audit (2026-06-28).**

The TLA+ model captures the high-level TM semantics (begin, read, write, commit with Percolator 2PC phases) but has significant simplifications: unbounded `committed`/`aborted` counters prevent TLC termination, asynchronous network behavior is abstracted to synchronous state transitions, TiKV internal Percolator protocol details are modelled at the wrong abstraction level (Prewrite/CommitPrimary/CommitSecondary are separate model actions but a single `txn.commit()` call in the code), and the panic-based TmxAbort retry mechanism is modelled as direct state transitions.

## Files

| File | Role |
|------|------|
| `docs/proofs/TiKV.tla` | TLA+ specification of the TiKV TM backend (345 lines) |
| `expli_instr/rust/workspace/runtime/tikv/src/lib.rs` | Rust implementation (406 lines) |
| `backends/tm_impl/tikv/tikv_backend.cpp` | C++ FFI shim (174 lines) |
| `backends/tm_impl/tikv/README.md` | Architecture documentation (98 lines) |

## Algorithm Summary

TiKV is a distributed TM backend wrapping TiKV (Percolator-style 2PC). TM addresses are mapped to TiKV keys via offset from the TM region start (`"tm:{region_offset:016x}"`). Reads use a write-set-before-read-set-before-TiKV-gRPC-get chain with lazy-fetch caching; writes are buffered in a local write-set and flushed atomically at commit via TiKV's Percolator 2PC. TiKV read errors (TxnNotFound from concurrent commit locks) trigger `TmxAbort` panic → retry loop; commit failures simply return `false` to the caller.

## Cross-Reference Checklist

### Rust/C++ → TLA+

| Rust Function | C++ FFI | TLA+ Action | Notes |
|---------------|---------|-------------|-------|
| `tikv_init()` / `tm_init()` | `tikv_tm_init()` | `Init` (state only) | Model has no init action beyond variable initialization; no PD connection or runtime setup |
| `tikv_begin()` / `tm_begin()` | `tikv_tm_begin()` | `Begin(t)` | Correct: starts TiKV optimistic snapshot, clears write/read-set |
| `tm_read_u*/tm_read_i*/etc` | `tikv_tm_read_u*()` | `Read(t, k)` + `TxnConflictRetry(t)` | Write-set check → read-set cache → TiKV get. Model handles TxnNotFound via conflict actions |
| `tm_write_u*/tm_write_i*/etc` | `tikv_tm_write_u*()` | `Write(t, k, v)` | Correct: buffers in write_set, sets primary key on first write |
| `tikv_commit()` | `tikv_tm_end()` | `ReadOnlyCommit(t)` / `Prewrite(t)` → `CommitPrimary(t)` → `CommitSecondary(t)` | **Deviations 2, 4, 8**: model decomposes TiKV's internal 2PC into 3+ actions; implementation calls single `txn.commit()` |
| `tikv_abort()` | (no C++ visible) | `Abort(t)` + `TxnConflictRetry(t)` + `TxnConflictAbort(t)` | Correct: releases locks, clears state, returns to idle |
| `tm_abort_count()` | — | (not modelled) | Always returns 0 |
| `tikv_read()` | — | `Read(t, k)` + conflict actions | Shared internal helper; write-set → read-set → TiKV get. Error → rollback + TmxAbort |
| `addr_key()` | — | (not modelled) | Key = `"tm:{region_offset:016x}"`. Model uses abstract `k \in Key` |
| `addrspace::tm_region_start()` | — | (not modelled) | Model assumes abstract key set, not address-based mapping |
| `runtime_core::tm_install_tmx_hook()` | — | (not modelled) | Sets panic hook to suppress TmxAbort output |
| `tm_register_real_hooks()` | `g_tikv_hooks` | (not modelled) | Hooks table registration |
| `LLVM_TM_PLUGIN` wrappers | `do_tm_init()` etc. | (not modelled) | Plugin DATA/TEXT symbol fix |

### TLA+ → Rust/C++

| TLA+ Action | Rust Equivalent | Fidelity |
|-------------|----------------|----------|
| `Init` | `tikv_init()` + `tm_region_init()` | Good (model skips PD connection details) |
| `Begin(t)` | `tikv_begin()` | Good |
| `Read(t, k)` | `tikv_read()` → `tm_read_u*()` | Good (write-set check, read-set cache, fetch) |
| `TxnConflictRetry(t)` | TmxAbort panic during read → `transaction()` retry loop | Simplified (model uses direct state transitions, not panic/unwind) |
| `TxnConflictAbort(t)` | (no Rust equivalent) | **Deviation 5**: model has permanent abort after MaxRetries; real code retries indefinitely |
| `Write(t, k, v)` | `tm_write_u*()` | Good |
| `ReadOnlyCommit(t)` | `tikv_commit()` when write-set is empty | Good |
| `Prewrite(t)` | `txn.commit()` internals (inside tikv-client) | **Deviation 2**: model exposes internal Percolator phase; real code treats commit as opaque |
| `PrewriteConflict(t)` | TiKV Percolator lock conflict during 2PC | Good (abort on lock conflict) |
| `CommitPrimary(t)` | Percolator primary key commit (inside txn.commit()) | **Deviation 2**: internal Percolator detail |
| `CommitSecondary(t)` | Percolator secondary key commit (inside txn.commit()) | **Deviation 2**: internal Percolator detail |
| `Abort(t)` | `tikv_abort()` | Good |

## Invariants

### TLC Result

**TLC does not terminate** due to unbounded `committed[t]` and `aborted[t]` (type `Nat`). After 90s with config `Thread={1}, Key={0}, Data={0,1}, MaxRetries=1`:
- ~24M states generated, ~6.7M distinct states found
- Worker queue still growing (12K+ entries)
- **No invariant violation found** in the explored portion

The unbounded counters allow the model to run infinite transaction sequences (Begin→Commit→Begin→Commit→...), each incrementing `committed[t]` with no upper bound. TLC explores a new distinct state on every increment.

**Recommended fix**: Change `committed[t]` and `aborted[t]` from `Nat` to a bounded range, or add a `MaxTransactions` constant gating the `Next` action.

| Invariant | TLC Result | Description |
|-----------|------------|-------------|
| `LockExclusion` | ✅ Pass (partial) | A key can be locked by at most one thread. Passes all explored states |
| `NoStaleLocks` | ✅ Pass (partial) | Idle threads hold no locks. Passes all explored states |
| `CommittedVisible` | ✅ Pass (partial) | Committed writes are visible in kv_store. Passes all explored states |
| `SnapshotIsolation` | ✅ Pass (partial) | Locked keys have valid data. Passes all explored states |
| `CommitOrdering` | ✅ Pass (partial) | Primary key precedes secondaries in commit. Passes all explored states |
| `NoDoubleCommit` | ✅ Pass (partial) | No key locked AND committed by different threads. Passes all explored states |

### Invariant definitions

| Invariant | Meaning | Source |
|-----------|---------|--------|
| `LockExclusion` | `∀k: ¬(∃t1,t2: locks[k]=t1 ∧ locks[k]=t2 ∧ t1≠t2)` | TLA+ line 307 |
| `NoStaleLocks` | `∀t: pc[t]="idle" ⇒ ∀k: locks[k]≠t` | TLA+ line 313 |
| `CommittedVisible` | `∀t,k: pc[t]="idle" ∧ HasWritten(t,k) ⇒ committed[t]>0 ∨ aborted[t]>0` | TLA+ line 319 |
| `SnapshotIsolation` | `∀k: locks[k]≠0 ⇒ kv_store[k]∈Data` | TLA+ line 326 |
| `CommitOrdering` | `∀t: pc[t]∈{"prewriting","committing"} ⇒ primary_key[t]∈Key ∨ read-only` | TLA+ line 332 |
| `NoDoubleCommit` | `∀k: locks[k]=0 ∨ ¬∃t: HasWritten(t,k) ∧ committed[t]>0 ∧ locks[k]=t` | TLA+ line 340 |

## Deviations

### 1. Unbounded `committed`/`aborted` counters prevent TLC termination

**Issue**: Both `committed[t]` and `aborted[t]` are declared as `[Thread -> Nat]` with no upper bound. Every successful `ReadOnlyCommit` or `CommitSecondary` increments `committed[t]`, and every `TxnConflictRetry`/`TxnConflictAbort`/`Abort`/`PrewriteConflict` increments `aborted[t]`. Since a thread can run infinitely many transactions, TLC generates distinct states for every possible counter value → state explosion.

**Fix**: Add `MaxTransactions` constant to bound the counters, or use a modulo approach.

**Severity**: High (verification blocker)

### 2. Percolator internal 2PC decomposition not present in implementation

**Issue**: The TLA+ model decomposes commit into `Prewrite` → `CommitPrimary` → `CommitSecondary` (3 separate model actions). The Rust implementation calls `state.txn.commit().await` — a single function call. The Percolator 2PC phases are handled inside the tikv-client library, not by this backend. The model is modelling TiKV internals rather than the backend code.

**Impact**: The model's `CommitOrdering` invariant is trivially true (the model enforces it). The real TiKV commit is opaque and correctness depends on TiKV's Percolator implementation.

**Severity**: Medium (model validates external library semantics, not the backend)

### 3. No async/network abstraction

**Issue**: The TLA+ model is purely synchronous (every action is a single atomic state transition). The real implementation uses `tokio::runtime::Runtime::block_on()` for every TiKV operation: `begin_optimistic()`, `get()`, `put()`, `commit()`, `rollback()`. gRPC latency, network partitions, timeouts, and async scheduling are all abstracted away.

**Impact**: Network failures, partial failures (e.g., prewrite succeeds on 3 of 5 keys then TiKV crashes), and timeout-based aborts are not modelled.

**Severity**: Medium (expected for a TLA+ model of a TM backend, but reduces confidence in network-fault scenarios)

### 4. TmxAbort panic-based retry is modelled as direct state transitions

**Issue**: In the real implementation, TiKV read errors call `std::panic::panic_any(TmxAbort)` which unwinds the stack to the `catch_unwind` in the TM crate's `transaction()` loop. The TLA+ model represents this as direct `TxnConflictRetry(t)` / `TxnConflictAbort(t)` actions that directly transition `pc[t]` and state variables — no stack unwind, no panic catch, no retry loop visible at the model level.

**Impact**: The model validates the state invariant (locks released, write-set cleared) but not the control-flow correctness (is the panic handler installed? Does `catch_unwind` properly catch only `TmxAbort`?).

**Severity**: Medium (invariant correctness is validated; control-flow correctness is not)

### 5. MaxRetries vs indefinite retry

**Issue**: The TLA+ model includes `TxnConflictAbort(t)` which triggers when `retry_count[t] >= MaxRetries` and permanently aborts the transaction. The real implementation's retry loop in the TM crate retries indefinitely (no upper bound in the backend; the TM crate's `transaction()` loops until success). The `MaxRetries` concept does not exist in the Rust code.

**Impact**: The model may explore "permanent abort" states that cannot occur in the real implementation. Conversely, the model does not explore infinite-retry behaviors.

**Severity**: Low (permanent abort is a conservative over-approximation)

### 6. No key encoding model

**Issue**: The real implementation maps TM addresses to TiKV keys via `format!("tm:{:016x}", offset)` where `offset = addr - tm_region_start()`. The TLA+ model uses an abstract set `Key` with no addressing or encoding logic. Cross-process key agreement (different processes with different virtual addresses must compute the same TiKV key) is not modelled.

**Impact**: Address collision or encoding errors would not be detected.

**Severity**: Low (key encoding is straightforward; abstract set is appropriate for TLA+)

### 7. `tm_abort_count()` returns 0

**Issue**: The Rust implementation hardcodes `tm_abort_count() -> u64 { 0 }`. The TLA+ model tracks `aborted[t]` as a counter. If the model's counter were used to answer `tm_abort_count()`, the answer would differ from the implementation (which always returns 0).

**Impact**: Backends that query `tm_abort_count()` would get wrong information from this backend, but none currently do for TiKV.

**Severity**: Low

### 8. Commit failure returns `bool`, not atomic 2PC in model

**Issue**: The Rust `tikv_commit()` returns `bool` (`true` = success, `false` = conflict/error). In the model, `PrewriteConflict` transitions to `"aborting"` rather than just returning `false`. The model's commit is an all-or-nothing sequence of 3 actions, while the real implementation's `txn.commit()` is a single opaque call that either succeeds or fails atomically. The model adds intermediate states (`"prewriting"`, `"committing"`) that don't exist in the code.

**Impact**: The model may fail to capture partial-commit failure modes (e.g., `txn.commit()` fails in the middle of 2PC). TiKV handles these internally with roll-forward recovery.

**Severity**: Low (TiKV handles 2PC recovery internally; the backend doesn't need to model intermediate states)

### 9. No TM memory allocator integration

**Issue**: The TLA+ model has no concept of `tm_malloc`, `tm_calloc`, `tm_free`, or `tm_region_*`. The real C++ shim uses `stm::tm_region_malloc()` for memory allocation. The model assumes all data lives in the abstract `kv_store`, not in a TM region.

**Impact**: Memory allocation patterns and their interaction with TiKV storage are not modelled.

**Severity**: Low (separate concerns — allocator correctness is independent of TiKV protocol)

### 10. `NoWrite == 0 - 1` is a TLA+ integer underflow

**Issue**: `NoWrite == 0 - 1` evaluates to `-1` (in standard integer TLA+), represented as the maximum natural number when extended. The model's `write_set[t][k]` is typed as `Data \cup {NoWrite}` in the state space, but `0 - 1` is not in `Data`. This works because TLC uses the Naturals module which extends to all integers. However, the type assertion `Data \subseteq Nat` in `ASSUME` does not prevent `NoWrite` from being a negative value.

**Impact**: Cosmetic — all comparisons (e.g., `HasWritten(t,k) == (write_set[t][k] # NoWrite)`) are correct because they use `#` (not-equal). But the type-level mismatch could confuse tooling.

**Severity**: Cosmetic

## Summary

| Dimension | Score | Notes |
|-----------|-------|-------|
| **Algorithmic Fidelity** | 4/5 | Begin/read/write/commit/abort flow captured correctly |
| **TLC Verifiability** | 2/5 | Unbounded counters prevent termination; invariants pass explored states |
| **Implementation Coverage** | 3/5 | Read/write/commit/abort modelled; async, key encoding, allocator omitted |
| **Concurrency Correctness** | 4/5 | Lock exclusion, no stale locks, commit ordering all validated (partial) |
| **Model Completeness** | 2/5 | Missing: async abstraction, TmxAbort panic mechanism, retry loop, memory allocator |
| **Documentation** | 4/5 | README accurately describes implementation; model comments describe simplification choices |

### Recommendations

1. **P0 — Bound `committed`/`aborted` counters**: Change to `[Thread -> 0..MaxTxCount]` and add `MaxTxCount` constant to make TLC terminate. This is the single change that would make full verification possible.

2. **P1 — Remove Percolator internal decomposition**: Replace `Prewrite`→`CommitPrimary`→`CommitSecondary` (3 actions) with a single `OpaqueCommit(t)` action. The Percolator phases are TiKV internals, not backend logic. A comment in the model should note that TiKV guarantees atomic 2PC.

3. **P2 — Model TmxAbort panic mechanism**: Even a simplified version (e.g., a shared `panic_signal` variable that `Read` can set, gating `Next`) would improve fidelity.

4. **P2 — Add `tm_register_global` / address range model**: The model should track the TM region base and verify key encoding uniqueness across threads.

### Suggested config for bounded TLC run

```
CONSTANTS
    Thread = {1, 2}
    Key = {1, 2, 3}
    Data = {0, 1, 2, 99}
    MaxRetries = 2
    MaxTxCount = 3       // ← new: bounds committed/aborted
```

With `MaxTxCount = 3`, each thread can execute at most 3 transactions, yielding a finite state space of approximately 50K–200K states.
