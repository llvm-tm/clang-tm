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

**Root cause**: The LLVM pass (`sigsetjmpName()` in `tm_platform.hpp`) returned `"__sigsetjmp"` on Linux, but `__sigsetjmp` is a real function (TEXT symbol) in glibc — the pass declares it as `external global ptr` (DATA symbol). Generated code: `load ptr, ptr @__sigsetjmp` reads 8 bytes of the function's machine code → garbage address → SIGSEGV. On macOS `sigsetjmpName()` returned `"tm_sigsetjmp"`, which the runtime defines as a proper `.quad` DATA symbol, so it worked.

**Fix**: `sigsetjmpName()` now returns `"tm_sigsetjmp"` on all platforms. Added `tm_sigsetjmp` DATA variable definition (as C-level `int (*tm_sigsetjmp)(void*, int) = ...`) to `plugin/runtime/tm_runtime.cpp` and `plugin/runtime/persistent.cpp` for all platforms (previously only on Apple via Mach-O asm).

**Result**: `make -C plugin run` passes on Linux x86_64 (Docker QEMU). All 18 plugin tests pass on both macOS arm64 and Linux x86_64.

## Plugin runtime TLS/stub cleanup

- `tm_runtime.cpp` and `persistent.cpp` now define all TLS variables (`tm_nested_call_counter`, `tm_longjmp_ret`, `tm_jmpbuf`, etc.) and all hook DATA variables directly, eliminating the need to link `backends/tm_impl/common/tm_hooks.cpp` for plugin runtimes.

## Known issues

- `test_stress_ds` has a pre-existing assertion failure on non-TM addresses (region-size bug unrelated to hooks refactoring)
- TinySTM `counter_mt` has the same pre-existing assertion failure
- `make plugin-benchmarks` / STAMP benchmarks fail with `tm_safe_map.hpp` header path issue (pre-existing, unrelated to plugin)
- **rbtree double‑free in TM region allocator** (`FATAL: double-free detected in TM`) — pre‑existing. Root cause: region allocator reuses addresses across transactions; `g_deferred_frees_set` (thread‑local) fires false‑positive when same address freed again in a different thread.
- **stmbench7 times out with >1 thread** — root cause: data race in `ts_multimap::lower_bound()` — `op_st5` drops `std::mutex` before iterating, leaving raw iterator into `tm_malloc`‑backed memory. Affects NOrec, TL2, SwissTM; TinySTM survives by chance (per‑object locking serializes the path).
- **LEFTRIGHT bank/ycsb multi-thread deadlock** — pre‑existing. Left-right barrier implementation deadlocks with >1 thread.
- **XTM rbtree segfault** — pre‑existing. Page-level private copy scheme conflicts with double-free detection or region boundary.
- **ROMULUS bank multi-thread correctness**: `bank -d 500 -a 128 -t 2` fails ("Money created/destroyed") consistently with ≥2 threads. Passes with 1 thread. Root cause: OCC write-back (Phase 4) vs. concurrent read from another thread's `read_word`. The reader sees a partially-written state (some addresses updated, others not yet) at old version numbers, which the subsequent validation cannot distinguish from a consistent snapshot. Fix: read-validate pattern — capture version entry BEFORE reading, read data, re-check version entry after reading. If version changed or lock bit set, abort. Verified: all 114 test_tx + 207 test_ds pass, bank multi-thread money conserved. ✅ FIXED (2026-06-15)

## Session 2026-06-15 — Read-validate fix + race checker + Rust/C++ alignment

### ROMULUS OCC read-validate fix

**Root cause**: `read_word()` in romulus.hpp captured the version-table entry BEFORE reading from memory, but never re-checked it afterwards. A concurrent commit could write-back between the version check and the data read, producing an inconsistent snapshot that validation could not detect.

**Fix**: Re-read the version entry AFTER reading from memory. If the entry changed (version bumped or lock bit set), abort. Pattern matches standard OCC read-validate protocol.

**Files changed**: `backends/tm_impl/romulus/romulus.hpp` — read_word now reads-validates (capture → read → re-check → record).

**Verification**: `test_tx` 114/114, `test_ds` 207/207, `bank -t 4 -d 500 -a 128` passes money conservation.

### test_queue_multi counter race

**Root cause**: `counter` was a plain `static int` (not TM-annotated) in a TX function accessed from 4 threads. The LLVM pass only instruments TM-tracked globals, so `counter += delta` was a plain (non-atomic) load/add/store — classic lost-update.

**Fix**: Changed to `static TM int counter` so the LLVM pass instruments the access.

**Files changed**: `tests/plugin/test_queue_multi.cpp`

### Race checker findings

Running `libTMRaceChecker.so` on all benchmarks revealed only false positives:
- **bank**: `total_non_transactional()` accesses TM globals by design (called after all threads join)
- **avltree**: Helper functions are reachable from TX, cloned+instrumented by pipeline; originals are dead code
- **STAMP**: `intruder_generate_packets()`, `yada_generate_mesh()` are called before threads start (single-thread init)
- **fix**: Removed debug `fprintf(stderr, ...)` in `benchmarks/plugin/bank/bank.cpp` that directly accessed the `bank` TM global from `do_transaction_work()`

### Rust XTM implementation note

**Issue**: Rust XTM used version-table OCC (same as Romulus) but claimed to implement the XTM page-granularity private-copy algorithm from ASPLOS 2006. The algorithms are completely different.

**Fix**: Updated the implementation note in `expli_instr/rust/workspace/runtime/xtm/src/lib.rs` to accurately describe the Rust version as version-table OCC and document the difference from C++ XTM.

### Rust XTM read-validate fix

Same OCC read-validate race as C++ Romulus: capture version AFTER reading from memory. Fixed by re-ordering to read version → read data → re-check version (same protocol as C++ Romulus fix).

### Rust benchmark alignment

- Added `--version`/`-V` ASCII art to `fuzz_counter` and `fuzz_bank` (matching C++)
- Fixed `fuzz_bank` default accounts from 16 to 64 (matching C++)

### All files modified

- `backends/tm_impl/romulus/romulus.hpp` — ROMULUS read-validate fix
- `tests/plugin/test_queue_multi.cpp` — TM annotation on counter
- `benchmarks/plugin/bank/bank.cpp` — removed debug fprintf with TM global access
- `README.md` — updated ROMULUS test status (114/114 pass)
- `AGENTS.md` — this session summary
- `expli_instr/rust/workspace/runtime/xtm/src/lib.rs` — XTM implementation note + read-validate fix
- `benchmarks/rust/src/bins/fuzz_counter.rs` — added --version ASCII art
- `benchmarks/rust/src/bins/fuzz_bank.rs` — added --version ASCII art, fixed default accounts (16→64)

## Session 2026-06-17 — Debug printf cleanup into patches/debug system

### Problem

Source files contained stray `fprintf(stderr, ...)` debug printfs that clutter output and are only useful during active debugging. These should live in `patches/debug/` patches, not in source.

### Files cleaned

| File | Printfs removed |
|------|----------------|
| `backends/tm_impl/tiny_stm/TinySTM_runtime.cpp` | Two `#ifndef NDEBUG` blocks printing max read-set/write-set on exit (plugin + non-plugin paths) |
| `backends/tm_impl/tiny_stm/tinystm_wbctl.hpp` | `#ifndef NDEBUG` ASSERT debug dump on lock version mismatch; `#ifdef DEBUG_WBCTL` corrupted-address detector |
| `backends/tm_impl/swisstm/SwissTM_runtime.cpp` | `print_stats()` function + `atexit` registration (SwissTM begin/end counts) |
| `backends/tm_impl/tm_region_allocator/tm_region_allocator.cpp` | `#ifndef NDEBUG` mmap region info printf |
| `tests/expli-api/test_stress_ds.cpp` | `"DEBUG rbtree_tx: tree=%p..."` printf |

### Debug patches created

Five new patches in `patches/debug/patches/` (each adds back the removed printfs):

- `002-tinystm-rs-ws-debug.patch` — TinySTM max read-set/write-set stats
- `003-tinystm-wbctl-debug.patch` — tinystm_wbctl ASSERT + DEBUG_WBCTL blocks
- `004-tm-region-alloc-debug.patch` — TM region allocator mmap info
- `005-test-stress-ds-debug.patch` — test_stress_ds DEBUG printf
- `006-swisstm-stats.patch` — SwissTM print_stats + atexit

All 6 patches (001–006) apply cleanly via `patches/debug/apply.sh`.
