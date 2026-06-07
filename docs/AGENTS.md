# Session Summary

## Latest Session (2026-06-07) — Post-restructure fixes + Rust addrspace guard

### Path fixes after repo restructure (commit 9c13c52)

The repo was restructured to follow Honorio's 5-pass decomposition, moving files around:
- `backends/runtimes/*` → `backends/tm_impl/*/`
- `llvm_tm_plugin/` → `plugin/`
- `plugin-benchmarks/` → `benchmarks/plugin/`
- `expli-benchmarks/` → `benchmarks/cpp/`
- `rust_tm_api/` → `expli_instr/rust/workspace/`
- `run_compare_all.sh` → `benchmarks/scripts/run_compare_all.sh`

All relative paths in Makefiles, Cargo.toml, and the runner script were stale after the move. Fixed:

| File | Fix |
|------|-----|
| `benchmarks/scripts/run_compare_all.sh` | `cd $(dirname $0)/../..` (run from repo root); `PLUGIN_STAMP_DIR`, `PLUGIN_TPCC_DIR`, `PLUGIN_STM7_DIR`, `EXPLI_DIR`, `RUST_DIR` paths |
| `benchmarks/rust/Cargo.toml` | `../rust_tm_api/tm` → `../../expli_instr/rust/workspace/tm` |
| `benchmarks/cpp/Makefile` | `../backends/` → `../../backends/` everywhere; added `-I$(abspath ../..)/expli_instr/cpp` for moved `tm_api.hpp` |
| `expli_instr/cpp/expli_tm_api` | Symlink `expli_instr/cpp/expli_tm_api → include` created (old include path `expli_tm_api/tm_api.hpp` still used by source files) |

### Runtime fixes for link-time symbol resolution

The plugin instrumentation pass injects calls to `tm_get_thread_state()` and the expli API references `tm_nested_call_counter`/`tm_longjmp_ret`. Some runtimes were missing these:

| Backend | Missing symbol | Fix |
|---------|---------------|-----|
| TSXSGL | `tm_get_thread_state()` | Added `tm_get_thread_state()` + `#include "../common/tm_thread_state.hpp"` + `TM_INCLUDES_tsxsgl` in `tm_pipeline.mk` |
| SingleGlobalLock | `tm_nested_call_counter`, `tm_longjmp_ret` | Added `__thread int32_t tm_nested_call_counter = 0; __thread int32_t tm_longjmp_ret = 0;` |

### Rust addrspace: auto-init guard for tm_region_malloc

**Root cause**: `TmCell::new()` calls `addrspace::tm_region_malloc()` directly (not through `spec_alloc()` which calls `ensure_region_init()`). If the first allocation happens before any explicit `tm_region_init()` call, the `OnceLock` accessors (`SC_BLOCK_SIZE.get().unwrap()`) panic on `None`.

**Fix**: Added a `REGION_START` guard at the top of `tm_region_malloc()` that calls `tm_region_init()` if the region hasn't been initialized yet.

**Before**: All 8 Rust STAMP benchmarks crashed with `panic at addrspace/src/lib.rs:603: called Option::unwrap() on a None value`.
**After**: All 8 pass with all backends (wbctl, norec, tsxsgl).

## Bugs from this session's run

Current benchmark run (`benchmark_run.log`, PID via `benchmarks/scripts/run_compare_all.sh`) running plugin/expli/rust × tsxsgl/tinystm_wbctl/norec/sgl × 8 STAMP + TPC-C + STMbench7, 12 threads × 3 samples + uninstrumented 1t.

### Crashes observed (>490 attempts, 451 completed)

| Benchmark | Backend | Impl | Severity | Details |
|-----------|---------|------|----------|---------|
| yada | tsxsgl | plugin | CRASH (sig 11) at 2t | Consistent segfault — pre-existing |
| yada | all | plugin | CRASH (all backends, ≥2t) | Pre-existing timing/topology issue |
| genome | tinystm_wbctl | plugin | SEGFAULT at all thread levels (1t+) | Output recovered (OK+) but signal 139 seen. ≤3t succeeds, ≥4t timeouts |
| genome | tinystm_wbctl | plugin | TIMEOUT at ≥4t | Benchmark hangs instead of completing within 600s |
| vacation | tinystm_wbctl | plugin | TIMEOUT at 4t, 10t, 21t, 56t | Also sporadic segfaults at ≥28t during cleanup |
| stmbench7 | tinystm_wbctl | plugin | CRASH | `std::vector::_M_realloc_insert` inside TM (known, all 36 runs skipped) |
| stmbench7 | norec | plugin | CRASH | Added to skip list |
| stmbench7 | — | plugin | BUILD FAIL | `stmbench_uninstrumented` — `#include "datastructures/tm_treap_map.hpp"` not found |

### Build failures

| Target | Error |
|--------|-------|
| `benchmarks/plugin/STAMP/Makefile stamp_tl2` | `tl2/tl2.hpp: No such file or directory` |
| `benchmarks/plugin/stmbench7/Makefile stmbench_uninstrumented` | `datastructures/tm_treap_map.hpp: No such file or directory` |

### Pre-existing unfixed issues (from prior sessions)

1. **High-concurrency segfaults (vacation, 28–56t)**: Sporadic SIGSEGV during thread cleanup after valid output. Affects all TinySTM backends (WBCTL, NOrec) plugin path. Metrics are trustworthy.
2. **Plugin stmbench7 vector crash**: `std::vector::_M_realloc_insert` inside TM region. Fundamental incompatibility.
3. **SSCA2 plugin SIGSEGV in `tm_begin` from worker thread**: Pre-existing, timing-dependent.
4. **stmbench7 + NOREC plugin cleanup hang ≥2t**: Added to skip list.

## Relevant Files
- `benchmarks/scripts/run_compare_all.sh` — Main benchmark runner
- `backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp` — Fixed: added `tm_get_thread_state()`
- `backends/tm_impl/single_global_lock/SingleGlobalLock_runtime.cpp` — Fixed: added thread-local counters
- `plugin/tm_pipeline.mk` — Fixed: added `TM_INCLUDES_tsxsgl`
- `expli_instr/rust/workspace/addrspace/src/lib.rs` — Fixed: auto-init guard in `tm_region_malloc`
- `benchmarks/rust/Cargo.toml` — Fixed: dependency paths
- `benchmarks/cpp/Makefile` — Fixed: includes and paths
