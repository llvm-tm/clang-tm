# TM API C++ — Project Summary

## Phase-switching backend swap

`tm_swap_runtime()` now supports runtime backend switching. Added `tm_get_real_hooks()` to retrieve registered hooks. The phase-switch test (`test_phase_switch.cpp`) demonstrates: swap from stubs to real TinySTM hooks mid-run, verify money conservation across phases.

## Dual-backend swap test (TinySTM ↔ NOrec)

`test_swap_backends.cpp` links both TinySTM and NOrec in a single binary, swapping between them at runtime. NOrec's symbols are renamed via `norec_wrapper.cpp` (`#define`-based) to avoid linker conflicts with TinySTM. The 3 retry-loop TLS variables (`tm_jmpbuf`, `tm_nested_call_counter`, `tm_longjmp_ret`) were moved to `tm_hooks.cpp` so both backends share them. `tm_swap_runtime()` now also updates `s_real_hooks` to prevent `tm_hook_init_thread()` from reverting the swap.

## Direct backend refactoring (hooks system)

All 6 direct backends (DUDETM, NVHTM, SPHT, DistributedSGL, PersistentSGL, TSXSGL) now use the hook system: removed their `extern "C"` definitions of hook functions, made them `static`, and register via `TMRealHooks` + `tm_register_real_hooks()`.

## Plugin dead code cleanup

Removed unused functions (`createHookCall`, `handleMemoryIntrinsic`, `hasTMGlobals`, `isSTLContainerFunction`, `collectDirectCalls`, `callsTransactionFunctions`, `transitivelyCallsTransactionFunctions`, `TMMethodInfo`), unused file `AtomicDoLower.cpp`, and unused runtime functions (`tm_get_type_string`, `tm_read_z`, `tm_write_z`, `consume_ptr`, commented-out `tm_setjmp`).

## Benchmark bug fixes

- **eigenbench crash**: `thread_exit()` was inside the `while (!g_stop.load())` loop; after the first iteration `current_tx` was cleared and the next `transaction()` call crashed in `norec::begin()`. Fixed by moving `thread_exit()` outside the loop.
- **rbtree double‑free**: `tm_insert()` frees `z` on duplicate key at line 132, but caller `worker()` at line 244 freed `z` again. Removed the duplicate free from the caller.

## LLVM race checker plugin

`plugin/passes/TMRaceCheckerPass.cpp` — a standalone `opt` pass (`-passes="tm-race-checker"`) that scans all non‑transaction functions for loads/stores to TM‑annotated globals. Reuses `tracesFromTMGlobal()` from `tm_local_vars.hpp` (same analysis as the instrumentation pipeline). Emits source‑location warnings suggesting `[[tm::shared]]` annotation.

The instrumentation plugin (`libTMInstrument.so`) now also warns about missing transaction annotations at build time via `checkMissingTransactionAnnotations()` in `tm_instrument_helpers.hpp`, called from `setupModulePass()`.

Usage:
```sh
opt-22 -load-pass-plugin=plugin/bin/libTMRaceChecker.so \
       -passes="tm-race-checker" myapp.bc -o /dev/null
```

## --version / ASCII art

`plugin/bin/tm-race-checker` (shell wrapper) and `fuzz-counter`/`fuzz-bank` (C++ benchmarks) support `--version` / `-V`. Each prints box‑drawing letter art:

- **tm-race-checker** — "RACE" (leading ━) + "CHECKER" (7‑block)  
- **fuzz-counter**, **fuzz-bank** — "FUZZ" (4‑block)

## TM memory debug allocation

`tm_region_check_leaks()` added to `tm_region_allocator.hpp`, gated by `-DTM_DEBUG_ALLOC`. When enabled, a per‑thread `unordered_map<void*,size_t>` tracks live allocations; `tm_region_check_leaks()` at exit prints any unfreed pointers.

## Bug fixes (usability round)

- Fixed all stale paths in scripts and docs (llvm_tm_plugin → plugin, backends/runtimes → backends/tm_impl).
- Unified LLVM requirement to 22 in CMakeLists.txt.
- Added `.github/workflows/ci.yml`.
- Removed stale `backends/runtimes/` comments from 9 source/doc files.
- Removed `plugin/bin/tm-race-checker` from git tracking (.gitignore covers `**/bin/`).
- Fixed `docs/REQUIREMENTS.md` directory tree and install instructions.
- Fixed `plugin/README.md` install instructions (./install.sh → ./tools/install-plugin.sh).
- Fixed `plugin/clang-tm` install.sh references.
- Upgraded Python 3.8+ check to hard failure in check-requirements.sh.

## Backend bug fixes

- **XTM read_word/write_word**: Removed `#ifdef LLVM_TM_PLUGIN` guard on `isTMAddress()` check so non-TM addresses (e.g. regular heap from `::operator new` in `TM<int*>::alloc()`) fall through to direct read/write instead of crashing on page-aligned `memcpy` overflow. Fixes test_tx crash ("corrupted double-linked list") and test_ds crash with XTM.
- **LEFTRIGHT read_word**: Added write-set lookup before reading from memory, so own writes within a transaction are visible to subsequent reads. Also added missing `#include <algorithm>` for `std::sort` in commit path.
- **ROMULUS read_word**: Same write-set lookup fix as LEFTRIGHT.
- **LEFTRIGHT/ROMULUS**: Added missing `#include <algorithm>`.

## ROMULUS rewrite: proper version-table OCC commit

Replaced the broken commit-time CAS (which reinterpreted every data address as an `atomic<uint64_t>` version slot, corrupting data on write-back) with a proper version-table-based OCC protocol:

1. **Separate version table**: `g_version_table[]` — 2^20 entries of `atomic<uint64_t>`, indexed by `(addr>>3) & mask`. Independent of data addresses.
2. **Commit lock**: `g_commit_lock` spinlock serializes the commit path.
3. **Protocol**: Validate (check `version ≤ tx->timestamp`) → acquire lock → re-validate → increment global clock → write-back → fence → update version entries (`store(commit_ts)`) → release lock.
4. **write-set lookup in read_word**: own writes visible to subsequent reads within the same transaction.
5. **Removed old_val capture**: write-set no longer captures old values (undo logging not needed for OCC).

## Verified passing (all backends)

Comprehensive smoke test: `test_tx` + `test_ds` on 10 backends (TINYSTM, WBETL, WT, NOREC, SWISSTM, TL2, SGL, XTM, LEFTRIGHT, ROMULUS).

| Backend   | `test_tx` | `test_ds` |
|-----------|-----------|-----------|
| TINYSTM   | 114/114   | 207/207   |
| WBETL     | 114/114   | 207/207   |
| WT        | 114/114   | 207/207   |
| NOREC     | 114/114   | 207/207   |
| SWISSTM   | 114/114   | 207/207   |
| TL2       | 114/114   | 207/207   |
| SGL       | 114/114   | 207/207   |
| XTM       | 114/114   | 207/207   |
| LEFTRIGHT | 114/114   | 207/207   |
| ROMULUS   | 114/114   | 207/207   |

## sigsetjmp DATA/TEXT symbol fix (CI Linux crash)

**Root cause**: The LLVM pass (`sigsetjmpName()` in `tm_platform.hpp`) returned `"__sigsetjmp"` on Linux, but `__sigsetjmp` is a real function (TEXT symbol) in glibc — the pass declares it as `external global ptr` (DATA symbol). Generated code: `load ptr, ptr @__sigsetjmp` reads 8 bytes of function's machine code → garbage address → SIGSEGV. On macOS `sigsetjmpName()` returned `"tm_sigsetjmp"`, which the runtime defines as a proper `.quad` DATA symbol, so it worked.

**Fix**: `sigsetjmpName()` now returns `"tm_sigsetjmp"` on all platforms. Added `tm_sigsetjmp` DATA variable definition (as C-level `int (*tm_sigsetjmp)(void*, int) = ...`) to `plugin/runtime/tm_runtime.cpp` and `plugin/runtime/persistent.cpp` for all platforms (previously only on Apple via Mach-O asm).

## Plugin runtime TLS/stub cleanup

- `tm_runtime.cpp` and `persistent.cpp` now define all TLS variables and all hook DATA variables directly, eliminating the need to link `backends/tm_impl/common/tm_hooks.cpp` for plugin runtimes.

## Known issues

- `test_stress_ds` has a pre-existing assertion failure on non-TM addresses (region-size bug unrelated to hooks refactoring)
- TinySTM `counter_mt` has the same pre-existing assertion failure
- `make plugin-benchmarks` / STAMP benchmarks fail with `tm_safe_map.hpp` header path issue (pre-existing, unrelated to plugin)
- **rbtree double‑free in TM region allocator** (`FATAL: double-free detected in TM`) — pre‑existing
- **stmbench7 times out with >1 thread** — data race in `ts_multimap::lower_bound()`
- **LEFTRIGHT bank/ycsb multi-thread deadlock** — pre‑existing
- **XTM rbtree segfault** — pre‑existing
- **ROMULUS bank multi-thread**: was fixed with read-validate pattern (2026-06-15)

## Session 2026-06-23 — Comprehensive audit: all 18 TLA+ models (Phases 1-3 complete)

### TinySTM model fidelity audit and improvements

Audited all 3 TinySTM TLA+ models against their C++ implementations. Key gaps found and fixed:

| Gap | Affected | Severity | Fix |
|-----|----------|----------|-----|
| No `endVersion` per-thread | WBCTL, WBETL, WT | High | Added `endVersion[t]`, `L_extend` label, validation against `endVersion[t]` |
| Monolithic commit (validate+write-back+unlock as one action) | WBETL | High | Split into `L_incClock` + `L_validateETL` + `L_writeBackETL` (matches C++ phases) |
| Monolithic commit (validate+unlock as one action) | WT | High | Split into `L_validateWT` + `L_unlock` + `L_abort` |
| Extend abort path skips lock release | WBETL | Critical (model bug) | Added `lock[a] := <<0, 0, lock[a][3]>>` before `state := "idle"` in L_extend failure |
| No memory ordering annotations | All | Medium | Added `lastFence[t]` tracking + `FenceFidelity` invariant |

### Verification results (TinySTM models)

| Backend | States (before) | States (after) | TLC result |
|---------|----------------|----------------|------------|
| WBCTL | 12K | 146K (+lastFence) | PASS ✅ |
| WBETL | 3.9K | 58K (+commit split + lastFence) | PASS ✅ |
| WT | 5.9M | N/A (parallel too large with lastFence) | Sequential PASS ✅ |

### Fence annotations added

For each TinySTM backend, a `lastFence[t]` variable (""/"acq"/"rel"/"sc") is set at points matching C++ fences:
- **Read**: `"sc"` — matches `atomic_signal_fence(seq_cst)` before version load
- **Write (lock acquire)**: `"acq"` — matches CAS acquire semantics
- **Commit (clock inc)**: `"sc"` — matches `atomic_thread_fence(seq_cst)` before clock read
- **Validate success**: `"sc"` — matches fence before re-reading read-set
- **Unlock after commit/abort**: `"rel"` — matches `atomic_signal_fence(release)` before lock release

`FenceFidelity`: `\A t \in Thread : writeSet[t] # {} => lastFence[t] # ""`

### Comprehensive Audit of All 18 Backends (2026-06-23)

Audited all remaining 12 unaudited backends. Final score distribution:

| Score | Count | Backends |
|-------|-------|----------|
| **5/5** | 1 | SGL |
| **4/5** | 5 | TinySTM_WBCTL, TinySTM_WBETL, TinySTM_WT, Romulus, XTM |
| **3/5** | 6 | TSXSGL, PersistentSGL, TL2, LEFTRIGHT, SwissTM, NOrec, TiKV, TSXSim |
| **2/5** | 3 | NVHTM, SPHT, SimEngine |
| **1/5** | 2 | DUDETM, DistributedSGL |

### TLC bugs found (expanded)

| Backend | Bug | Fixed? |
|---------|-----|--------|
| TinySTM_WBETL | Write-conflict abort didn't release locks | ✅ (PlusCal) |
| TinySTM_WBETL | Extend abort path skipped lock release | ✅ (PlusCal) |
| SPHT | `DurableValid` fails: read-only TX triggers GroupCommit with empty PCL | ❌ C++ lacks guard |
| TSXSim | `TSXvsSGLSafety` fails: SGL begin while TSX active | ❌ Model bug (HW prevents via cache-coherence) |
| NVHTM | `FreshLogOnBegin` + `CommitPhaseOrdering` fail | ❌ Model bugs (invariant wording, self-violation) |

### Key findings by backend

- **NOrec (3/5)**: Plugin-mode bypass paths not modeled; clock double-check abstracted. Known bug from 2026-06-23 audit still unaddressed in model.
- **DUDETM (1/5)**: Worst fidelity. TLA+ is a high-level design sketch; actual impl is TinySTM WBCTL wrapper with forked replayer + swapped op-types. Fundamentally different algorithm.
- **NVHTM (2/5)**: TLA+ models checkpoint/recovery protocol that doesn't exist in C++; no SGL fallback in C++ (RTM failure→pass-through, not mutex); logging is dedup not append.
- **SPHT (2/5)**: `DurableValid` invariant fails; TSX retry model vs C++ no-retry; SGL PCL divergence; crash/recovery modeled but absent in C++.
- **DistributedSGL (1/5)**: TLA+ models client-server lock server with message-passing; C++ is single-machine file-backed mmap spinlock.
- **TiKV (3/5)**: Unbounded counters prevent TLC termination; Percolator 2PC decomposed vs single `txn.commit()`.
- **TSXSim (3/5)**: `TSXvsSGLSafety` fails; hardware cache-coherence prevents in practice.
- **SimEngine (2/5)**: Critical naming mismatch — `SimEngine.tla` (now `DESEngine.tla`) models DES `engine.rs`, not `sim_engine.rs` replayer.

### Fence annotation sweep (2026-06-24)

Added `lastFence[t]` + `FenceFidelity` to all 6 remaining PlusCal backends:

| Backend | States (config) | Result |
|---------|----------------|--------|
| **TSXSGL** | 840K / 99K (parallel) | PASS ✅ |
| **TL2** | 4 (sequential) | PASS ✅ |
| **XTM** | 225K / 37K (parallel) | PASS ✅ |
| **LEFTRIGHT** | 73 / 42 (sequential) | PASS ✅ |
| **SwissTM** | 3.5M / 699K (parallel) | PASS ✅ |
| **Romulus** | 1.79M / 440K (parallel) | PASS ✅ |

Fence points per backend (matching TinySTM pattern):
- **Read**: `"sc"` — signal fence before version capture
- **Lock acquire / first write**: `"acq"` — CAS acquire semantics
- **Clock increment**: `"sc"` — thread fence before clock
- **Validate success**: `"sc"` — fence before re-read
- **Unlock / commit**: `"rel"` — release fence before unlock
- **Abort**: `"rel"` — release before clean-up

Fence annotations now cover **9 backends** (all PlusCal specs).

**Known limitation:** `lastFence[t]` is a coarse approximation. It cannot distinguish `atomic_signal_fence` (compiler barrier) from `atomic_thread_fence` (CPU `dmb`/`mfence`), nor bundled RMW+ordering (`fetch_add(acq_rel)`). `FenceFidelity` only checks `writeSet ≠ {} ⇒ fence happened` — no guarantee of *sufficient* strength or *correct* placement. A proper memory-model proof would need `CAT`/`herd7`. These tags are documentation/consistency aids, not formal verification.

Score updates: TSXSGL 3→**4/5**, TL2 3→**4/5**, LEFTRIGHT 3→**4/5**, SwissTM 3→**4/5**. Romulus and XTM remain at 4/5.

### Files created/modified (2026-06-23 to 2026-06-24)
- `docs/proofs/tinystm_*.tla` — fence annotations, endVersion, L_extend, commit split
- `docs/proofs/{TSXSGL,TL2,XTM,LEFTRIGHT,SwissTM,Romulus}.tla` — fence annotations
- `docs/audits/*.md` — 18 audit reports (all backends)
- `docs/audits/SUMMARY.md` — all scores, bugs, observations, fence updates
- `docs/AGENTS.md` — this session summary

### TinySTM model fidelity audit and improvements

Audited all 3 TinySTM TLA+ models against their C++ implementations. Key gaps found:

| Gap | Affected | Severity | Fix |
|-----|----------|----------|-----|
| No `endVersion` per-thread | WBCTL, WBETL, WT | High | Added `endVersion[t]`, `L_extend` label, validation against `endVersion[t]` |
| Monolithic commit (validate+write-back+unlock as one action) | WBETL | High | Split into `L_incClock` + `L_validateETL` + `L_writeBackETL` (matches C++ phases) |
| Monolithic commit (validate+unlock as one action) | WT | High | Split into `L_validateWT` + `L_unlock` + `L_abort` |
| Extend abort path skips lock release | WBETL | Critical (model bug) | Added `lock := ... <<0, 0, lock[a][3]>>` before `state := "idle"` in L_extend failure |
| No memory ordering annotations | All | Medium | Added `lastFence[t]` tracking + `FenceFidelity` invariant |

### Verification results

| Backend | States (before) | States (after) | TLC result |
|---------|----------------|----------------|------------|
| WBCTL | 12K | 146K (+lastFence) | PASS ✅ |
| WBETL | 3.9K | 58K (+commit split + lastFence) | PASS ✅ |
| WT | 5.9M | N/A (parallel too large with lastFence) | Sequential PASS ✅ |

### Fence annotations added

For each backend, a `lastFence[t]` variable (""/"acq"/"rel"/"sc") is set at points matching C++ `atomic_signal_fence`/`atomic_thread_fence`/CAS:
- **Read**: `"sc"` — matches `atomic_signal_fence(seq_cst)` before version load
- **Write (lock acquire)**: `"acq"` — matches CAS acquire semantics
- **Commit (clock inc)**: `"sc"` — matches `atomic_thread_fence(seq_cst)` before clock read
- **Validate success**: `"sc"` — matches `atomic_thread_fence(seq_cst)` before re-reading read-set
- **Unlock after commit/abort**: `"rel"` — matches `atomic_signal_fence(release)` before lock release
- **Extend failure / abort**: `"rel"` — matches release fence before unlock in abort_tx

`FenceFidelity` invariant: `\A t \in Thread : writeSet[t] # {} => lastFence[t] # ""` — checks every lock-holding thread has done at least one fence.

### Files modified
- `docs/proofs/tinystm_wbctl.tla` — PlusCal + TLA+: added `endVersion`, `L_extend`, `lastFence`, `FenceFidelity`
- `docs/proofs/tinystm_wbetl.tla` — PlusCal + TLA+: added `endVersion`, `L_extend`, `lastFence`, split commit into 3 labels, fixed extend abort lock leak
- `docs/proofs/tinystm_wt.tla` — PlusCal + TLA+: added `endVersion`, `L_extend`, `lastFence`, split commit into `L_validateWT` + `L_unlock` + `L_abort`
- `docs/audits/tinystm_wbctl.md` — Updated with new gaps, scores, and fence analysis
- `docs/audits/tinystm_wbetl.md` — Updated with new gaps, scores, and fence analysis
- `docs/audits/tinystm_wt.md` — Updated with new gaps, scores, and fence analysis
- `docs/audits/SUMMARY.md` — Updated scores: WBCTL 3/5→4/5, WBETL 3/5→4/5, WT 4/5 (confirmed)
- `docs/AGENTS.md` — This session summary

### Next steps
1. Add `lastFence` + `FenceFidelity` to remaining backends (SGL, TSXSGL, TL2, XTM, LEFTRIGHT, SwissTM)
2. Phase 3 audit: TLA+-only backends (NOrec, DUDETM, NVHTM, SPHT, DistributedSGL, TiKV)
3. Consider increasing TLC heap for WT parallel model with fence tracking

## Previous sessions

See AGENTS.md for full session history including:
- Session 2026-06-22: Simulator calibration against real C++ NOrec
- Session 2026-06-21: Simulator cost mode + machine profile calibration + TSX simulation backend
- Session 2026-06-20: `.tm_shared` section, TiKV backend, SPHT SGL fallback, LEFTRIGHT OCC correctness fix
- Session 2026-06-17: Debug printf cleanup into patches/debug system
- Session 2026-06-15: Read-validate fix + race checker + Rust/C++ alignment
- Previous sessions: STAMP benchmarks, Honorio pipeline, plugin fixes, etc.
