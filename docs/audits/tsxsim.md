# Audit: TSXSim (Bloom-filter-based TSX Simulation Backend)

**Score: 2/5** — Virtual cycle counting replaces real memory ordering; bloom filter false-positive rate configurable but not modeled; capacity thresholds abstracted. No `lastFence` tracking. Model captures dual-path TSX/SGL at high level. **Downgraded from memory ordering audit (2026-06-28).**

## Files

| Artifact | Path | Lines |
|----------|------|-------|
| TLA+ spec | `docs/proofs/TSXSim.tla` | 503 |
| Rust implementation | `explicit_api/rust/workspace/runtime/tsx_sim/src/lib.rs` | 997 |
| Cargo manifest | `explicit_api/rust/workspace/runtime/tsx_sim/Cargo.toml` | 15 |
| TLC config | `docs/proofs/TSXSim.cfg` | 20 |

## Algorithm Summary

TSXSim models Intel TSX (RTM) at cache-line granularity using a bloom-filter read-set (4096 bits, double-hashing) for conflict detection, an exact HashMap write-set, configurable capacity limits (Skylake defaults: 128 read lines / 32 write lines), and a virtual cycle-counter cost model calibrated against real Broadwell-EP TSX measurements. On capacity overflow, retry exhaustion, or conflict detection, it falls back to single-global-lock (SGL) mode. The simulator uses the same `TmRealHooks` API as other backends and can be driven by the DES engine or standalone.

## Cross-Reference Checklist

| Rust function | TLA+ label/action | Match | Notes |
|---|---|---|---|
| `tm_init()` | — | ✅ | Global env-var config read; no TLA+ equivalent (TLC uses CONSTANTS) |
| `tm_exit()` | — | ✅ | Stats print; not modelled |
| `tm_init_thread()` / `tm_exit_thread()` | — | ✅ | Per-thread state init; implicit in TLA+ (state exists at Init) |
| `try_begin_single()` | `TSXBegin(t)` | ⚠️ Partial | Both check `sgl_lock=0` / `SGL_OWNER==0`. Rust charges `COST_XBEGIN + COST_XABORT` on LOCK_BUSY (two-step: xbegin+xabort); TLA+ charges only `COST_XBEGIN` (20 cycles, no abort cost). |
| `tm_begin()` retry loop | `TSXBegin(t)` + implicit retry (Next state) | ⚠️ Partial | TLA+ models retries as separate `TSXConflictAbort/TSXRetry` actions choosing non-deterministically; Rust has a deterministic loop. Cross-transaction persistent retry counter in Rust is artificial (SimEngine adaptation). |
| `tm_commit()` — conflict detection | `TSXCommit(t)` + `ConflictFree(t)` | ✅ | Bloom-filter WR check + write-set WW check. Both check all active TSX threads. |
| `tm_commit()` — OWNER_CHANGED | `TSXCommit(t)`: `sgl_lock = 0` | ⚠️ Partial | Rust checks `SGL_OWNER != 0`; TLA+ checks `sgl_lock = 0`. Both check current value only, not whether it changed since begin. Mutual-exclusion sequence (SGL begin→end before TSX commit) bypasses check in both. |
| `tm_commit()` — WW conflict abort | `TSXCommit(t)` via `ConflictFree(t)` | ✅ | Both abort both writers on write-set overlap. |
| `tm_commit()` — WR conflict abort | `TSXCommit(t)` via `ConflictFree(t)` | ✅ | Rust aborts the reader; TLA+ prevents writer commit. Semantically equivalent (reader loses, writer wins). |
| `tm_commit()` — SGL path | `SGLCommit(t)` | ✅ | Releases lock, no conflict detection. |
| `tm_abort()` | `TSXConflictAbort(t)` + `TSXRetry(t)` | ✅ | Reset state, increment retry/abort counters, charge xabort cost. |
| `tm_read_u*`/`tm_read_i*` — TSX path | `TSXRead(t, a)` + `TSXReadCapacityAbort(t, a)` | ⚠️ Partial | Both insert into bloom + read_lines, check capacity. Rust charges `COST_READ_L1 + COST_BLOOM_CHECK`; TLA+ charges `4` (L1 read). TLA+ does not charge bloom-check cost separately. |
| `tm_write_u*`/`tm_write_i*` — TSX path | `TSXWrite(t, a)` + `TSXWriteCapacityAbort(t, a)` | ✅ | Both buffer write, track cache line, check capacity, simulate RMW (bloom update for write also). |
| `tm_read_raw` / `tm_write_raw` | — | 🟢 | Raw memcpy ops not modelled in TLA+ |
| `sim::try_begin()` | `TSXBegin(t)` | ✅ | SimEngine entry point — same semantics |
| `sim::force_sgl()` | `TSXFallback(t)` | ✅ | Acquires SGL, sets in_tsx=false |
| `sim::snapshot_states()` / `restore_states()` | — | 🟢 | Simulation infrastructure; not relevant to TLA+ model |
| `SGL_OWNER` load → `try_begin_single` | `SGLBegin(t)`: `sgl_lock=0` | ❌ **Gap** | Rust checks `SGL_OWNER` (AtomicU64, any thread ID). TLA+ `sgl_lock` uses thread ID. Rust's check correctly rejects LOCK_BUSY during begin; TLA+ model ALLOWS SGL to overlap TSX because SGLBegin does NOT check `mode[t2] # "tsx"` for other threads. |
| Write-after-write read-from-write-set | — | 🟢 | Rust reads own writes via write-set scan; TLA+ model doesn't model data values for TSX reads (no read value returned). |

## Invariants

TLC result: **FAIL** — `TSXvsSGLSafety` violated after 25 states (depth 3).

| Invariant | TLC result | Description |
|---|---|---|
| `LockFreeInv` | ✅ Pass | `sgl_lock = 0` ↔ no thread in SGL mode |
| `LockOwnerInv` | ✅ Pass | Lock owner thread always in SGL mode |
| `TSXvsSGLSafety` | ❌ **FAIL** | `mode[t] = "tsx"` ⇒ `sgl_lock = 0`. Violated trace: Thread 1 → TSXBegin, Thread 2 → SGLBegin (sgl_lock = 2). SGLBegin does not check other threads' modes. |
| `BloomContainsReads` | ✅ Pass | Bloom filter covers all `read_lines` entries |
| `CapacityBounds` | ✅ Pass | read/write-set sizes within limits during TSX |
| `WriteSetConsistent` | ✅ Pass | `write_set` ↔ `write_data` bidirectional consistency |
| `NoTSXCommitConflict` | ✅ Pass | (after parentheses fix) Conflicting TSX transactions cannot both commit |
| `NoSGLTSXOverlap` | ✅ Pass | SGL write-set and TSX write-set do not overlap |
| `LockExclusion` | ✅ Pass | At most one thread holds `sgl_lock` at a time |

### Invariant Violation Trace (TSXvsSGLSafety)

```
State 1: Init (both idle, sgl_lock=0)
State 2: t=1 → TSXBegin (mode[1]="tsx", pc[1]="tsx", cycles=20)
State 3: t=2 → SGLBegin (mode[2]="sgl", sgl_lock=2, cycles=100)
        → VIOLATION: mode[1]="tsx" ∧ sgl_lock=2
```

**Root cause:** `SGLBegin` precondition checks only `sgl_lock = 0`. It does not check whether any other thread is in TSX mode. Real hardware would detect the concurrent SGL mutex write via L1 cache-coherence (the mutex variable is in the TSX read-set), but the TLA+ model does not track `sgl_lock` as a read-set address.

## Deviations

1. **TSXvsSGLSafety gap (HIGH severity).** The TLA+ model allows TSX and SGL to coexist for one step (thread 1 enters TSX, then thread 2 enters SGL). Real TSXSGL would never allow this: the SGL mutex write (from `sgl_lock.store(1)`) would cause thread 1's TSX to abort on the next load/commit. The TLA+ model does not model the mutex variable as part of the tracked address space, so it cannot detect this. **Fix:** Add `\A t2 \in Thread : mode[t2] # "tsx"` as a guard in `SGLBegin`.

2. **No OWNER_CHANGED vulnerability (MEDIUM severity).** Both the Rust implementation and the TLA+ model check `SGL_OWNER == 0` / `sgl_lock == 0` only at commit time, not whether the value *changed* since begin. If a concurrent thread acquires and releases the SGL during another thread's TSX transaction, the commit passes despite potential data invalidation. The Rust code notes this as a deliberate simplification for SimEngine's sequential event ordering. Real TSXSGL stores `tsx_start_owner` at begin time and checks `sgl_owner != tsx_start_owner` at commit.

3. **No `cl` tracking for lock variable in conflict detection (MEDIUM severity).** The TLA+ model defines `Addr = {1..8}` for TM data, but `sgl_lock` is a separate variable. In real hardware, the SGL mutex address is part of the TSX transaction's read-set; any write to it (another thread acquiring SGL) causes an abort. The model cannot capture this, which is the root cause of deviation 1.

4. **No data-value tracking in TSX reads (LOW severity).** The TLA+ `TSXRead` action does not return a value or update `mem`. It only tracks the cache line in `read_lines` and `bloom`. The Rust implementation reads the actual address value. This is a simplification for model checking (reduces state space) and is acceptable.

5. **Capacity abort in Rust triggers on overflow, TLA+ allows overflow then aborts (LOW severity).** Rust's `def_read!` checks capacity *after* inserting the line, and on overflow sets `active = false`. TLA+ has a separate `TSXReadCapacityAbort` action that fires *instead of* `TSXRead` when `Cardinality > MAX_READ_LINES`. Semantically equivalent (overflow ≠ success), but the action structure differs.

6. **TLA+ NoTSXCommitCounter invariant had parsing bug (trivial).** Initial formulation lacked parentheses around the `\E` body, causing spurious initial-state violation. Fixed during audit by wrapping in `(t1 # t2 /\ \E ...) => ...`.

7. **Hash function is hardcoded to 6-bit bloom (resolved for model).** TLA+ uses a 6-bit bloom with 2 hash functions (fixed mapping per cache line). Rust uses 4096-bit bloom with multiplicative hashing. This is appropriate: the TLA+ model is a *symbolic abstraction* that verifies conflict-detection *soundness* (bloom has no false negatives), not performance fidelity.

8. **Bloom filter does not model false positives (intentional).** Both TLA+ and Rust abstract bloom false positives away for conflict detection. The Rust implementation supports false positives (probabilistic) but the conflict-detection logic treats "might contain" as a positive. The TLA+ `ConflictFree` predicate checks the exact `read_lines` set, not the bloom, so it has no false positives. This is conservative for safety verification.

9. **No concurrency in bloom-filter check at commit (architectural).** Rust serializes the entire commit (including conflict scanning) under `GLOBAL_STATE.lock()`. The TLA+ model's `ConflictFree(t)` checks all other threads in a single atomic step. Real TSX hardware detects conflicts via distributed cache-coherence (MESI). The serialization is appropriate for the SimEngine's event-driven model but doesn't capture the hardware's true parallelism.

## Summary

| Dimension | Rating | Notes |
|---|---|---|
| Fidelity to Rust impl | 3/5 | Core algorithm matches; key gap in TSX↔SGL mutual exclusion (deviation 1) |
| Fidelity to real TSX | 2/5 | Bloom filter approximates L1 tracking; serialized conflict detection ≠ hardware coherence; no L1 associativity modelling |
| TLC verification | 2/5 | 1 of 9 invariants FAIL; state space limited (25 states, depth 3) — tiny parameterization (2 threads, 8 addresses, 4 cache lines) |
| Proof structure | 4/5 | 9 invariants, 4 theorems, 1 liveness property; well-commented |
| Rust code quality | 4/5 | 997 lines, clean macro-driven read/write API, documented simplifications, env-var configuration |
| Documentation | 3/5 | Module-level docstring in Rust covers all key design points; TLA+ header has good algorithm summary. SGL fallback/virtual cycle counter both documented. |

**Key recommendation:** Fix `SGLBegin` to guard against concurrent TSX execution by adding `\A t2 \in Thread : mode[t2] # "tsx"`. Consider adding an `OWNER_CHANGED` variable to properly model the begin-time vs commit-time SGL owner check. This would likely improve state space but significantly increase model fidelity.
