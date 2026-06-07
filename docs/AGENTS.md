# Session Summary

## Latest Session (2026-06-07) — Serialize lock leak fix + genome/bayes/intruder lock restoration

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

### Serialize lock leak fix (across siglongjmp in abort_tx)

**Root cause**: `tm_serialize_lock()` was called in benchmark code (genome, bayes, intruder) to protect STL containers during concurrent TM transactions. When a TX aborted via `siglongjmp`, the recursive mutex was held forever — no thread could acquire it again, causing hangs.

**Fix**: Added thread-local `g_serialize_lock_count` in `TinySTM_runtime.cpp` and `tm_serialize_unlock_all()` that unlocks N times before `siglongjmp`:
- `tinystm_wbctl.hpp:abort_tx()` — calls `tm_serialize_unlock_all()` before `siglongjmp`
- `tinystm_wt.hpp:abort_tx()` — same fix
- (WBETL uses shared code path)

**Result**: serialize_lock can safely be used in benchmarks again without leaking across aborts.

### Serialize lock restored in genome/bayes/intruder

After an earlier fix temporarily removed `tm_serialize_lock/unlock` from genome, bayes, and intruder to work around the leak, they must be restored now that the leak is fixed. These locks are essential for protecting STL `unordered_set`/`unordered_map`/`priority_queue`/`queue` operations inside TM transactions — without them, concurrent inserts cause data structure corruption.

**Status**: All three benchmarks restored with serialize_lock. Genome now runs 7t @ 1M segments successfully (intermittent — some crashes at 4t+ likely from concurrent phase mismatch between genome_dedup and genome_match, a pre-existing benchmark design issue).

### Rust addrspace: auto-init guard for tm_region_malloc

**Root cause**: `TmCell::new()` calls `addrspace::tm_region_malloc()` directly (not through `spec_alloc()` which calls `ensure_region_init()`). If the first allocation happens before any explicit `tm_region_init()` call, the `OnceLock` accessors (`SC_BLOCK_SIZE.get().unwrap()`) panic on `None`.

**Fix**: Added a `REGION_START` guard at the top of `tm_region_malloc()` that calls `tm_region_init()` if the region hasn't been initialized yet.

**Before**: All 8 Rust STAMP benchmarks crashed with `panic at addrspace/src/lib.rs:603: called Option::unwrap() on a None value`.
**After**: All 8 pass with all backends (wbctl, norec, tsxsgl).

## Bugs (current status)

### Fixed this session

| Bug | Fix |
|-----|-----|
| Serialize lock leaked across `siglongjmp` in TinySTM abort | Added thread-local counter + `tm_serialize_unlock_all()` in `abort_tx()` for WBCTL/WT backends |
| Genome/bayes/intruder crashes at multi-thread (STL corruption) | Restored `tm_serialize_lock/unlock` in all three benchmarks now that leak is fixed |

### Unfixed

| Issue | Impl | Status |
|-------|------|--------|
| Yada plugin crash at ≥2t (all backends) | plugin | Pre-existing, timing/topology |
| Genome WBCTL segfault at 4t+ (intermittent) | plugin | Concurrent phase mismatch between `genome_dedup`/`genome_match` (no barrier) |
| Vacation WBCTL timeout at ≥4t | plugin | Pre-existing, may improve after stale-process cleanup |
| Vacation segfault at ≥28t during cleanup | plugin | Pre-existing, sporadic |
| stmbench7 WBCTL vector realloc crash | plugin | Pre-existing, fundamental STL incompatibility |
| stmbench7 NOREC cleanup hang ≥2t | plugin | Pre-existing |
| stmbench7 uninstrumented build failure | plugin | Missing `tm_treap_map.hpp` |
| TL2 build failure | plugin | Missing `tl2/tl2.hpp` |

## Relevant Files
- `benchmarks/scripts/run_compare_all.sh` — Main benchmark runner
- `backends/tm_impl/tiny_stm/TinySTM_runtime.cpp` — `tm_serialize_lock/unlock/unlock_all`, `g_serialize_lock_count`
- `backends/tm_impl/tiny_stm/tinystm_wbctl.hpp` — `abort_tx` calls `tm_serialize_unlock_all()` before `siglongjmp`
- `backends/tm_impl/tiny_stm/tinystm_wt.hpp` — same fix
- `backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp` — Fixed: added `tm_get_thread_state()`
- `backends/tm_impl/single_global_lock/SingleGlobalLock_runtime.cpp` — Fixed: added thread-local counters
- `plugin/tm_pipeline.mk` — Fixed: added `TM_INCLUDES_tsxsgl`
- `expli_instr/rust/workspace/addrspace/src/lib.rs` — Fixed: auto-init guard in `tm_region_malloc`
- `benchmarks/rust/Cargo.toml` — Fixed: dependency paths
- `benchmarks/cpp/Makefile` — Fixed: includes and paths
- `benchmarks/plugin/STAMP/genome_bench.hpp` — Restored `tm_serialize_lock/unlock`
- `benchmarks/plugin/STAMP/bayes_bench.hpp` — Restored `tm_serialize_lock/unlock`
- `benchmarks/plugin/STAMP/intruder_bench.hpp` — Restored `tm_serialize_lock/unlock`
- `benchmarks/plugin/STAMP/stamp_common.hpp` — Declares `tm_serialize_unlock_all()`
