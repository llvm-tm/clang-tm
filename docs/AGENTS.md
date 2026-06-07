# Session Summary

## Latest Session (2026-06-07) — Detailed TM metrics + bayes 4-thread hang fixed + Table VI comparison

### Fixes applied to plugin STAMP benchmarks

All benchmarks now use flat arrays instead of STL containers inside `struct TM`/`TX` functions, since the LLVM TM pass misbehaves with opaque libstdc++ calls and TinySTM intercepts `operator new` inside transactions.

| Benchmark | Bug | Fix |
|-----------|-----|-----|
| **kmeans** | Nested `std::vector<std::vector<double>>` caused 0ms/0 aborts | Flat `double*` arrays; TM-wrapped aggregate-update |
| **vacation** | 4 `std::map<int,T>` caused 1000× aborts | Flat `T*` arrays indexed by id-1 |
| **genome** | `std::string`/`std::unordered_set` caused bad_alloc/timeout | `char*`/`char**` arrays; sorted-array dedup; hash-table built outside TX |
| **intruder** | `std::queue`/`std::unordered_map`/`std::string` caused mmap crash | Ring buffer; inline char buffers; flat decoder-flow array |
| **bayes** | `std::vector<std::vector<int>>`/`std::set` caused `std::length_error` | Flat `int*` arrays; alloca BFS; stack density counters |
| **yada** | `std::set`/`std::vector` in TX + missing `stop_workers` + wrong border-edge dedup → timeout | BFS moved to THREAD; sort/dedup by cancel-pairs; `empty_count>=3` break |
| **SSH** | Input file format; plugin generates synthetic data via `-j` flag | Uses synthetic mesh when no `-i` given |

### Fixes applied in this session

| Fix | Details |
|-----|---------|
| **Bayes 4-thread hang** | `compute_density_ll` not marked `TX`+`tm_allow_opaque` — LLVM pass instrumented its loads/stores incompatibly with serialize lock held by callers. Added `TX`, `tm_allow_opaque`, and `tm_serialize_lock/unlock`. **Result**: 236ms at 4t, 0 aborts |
| **Missing serialize unlock** | `compute_density_ll` returned without `tm_serialize_unlock()` — lock leaked on every call from `find_best_insert`/`apply_insert`. Added unlock before return |
| **Detailed TM metrics** | New globals (`g_tm_min_read_set`, `g_tm_min_write_set`, `g_tm_total_commit_reads`, `g_tm_total_commit_writes`, `g_tm_commit_count`) tracked per-commit in `tm_end()`, printed as `TM_STATS:` at `tm_exit()` |
| **Table VI comparison** | `run_profiling.py` now compares measured avg/min/max read/write against OCR'd Table VI (90th pctile columns). CSV results saved to `patch/profile/results_tinystm_4t_detailed.csv` |

### Detailed TM metrics (tinystm, 4 threads, paper params)

| Benchmark | Time | Commits | AvgR | MinR | MaxR | AvgW | MinW | MaxW | Aborts |
|-----------|------|---------|------|------|------|------|------|------|--------|
| **bayes**       | 236ms |  1028 |  2016.0 |   1 |  2056 |   3.9 |   0 |    4 |     0 |
| **genome**      | 151ms |    10 | 46080.2 |  38 | 78055 |  97.0 |   0 |  481 |     8 |
| **intruder***   |   2ms |   —   |   —     |  —  |   —   |   —   |   — |   —  |    —  |
| **kmeans-high** |1442ms |   623 |  6266.7 | 741 |  9237 | 674.7 | 464 |  784 |   569 |
| **kmeans-low**  |1542ms |   603 |  6129.2 | 741 |  9237 | 672.7 | 464 |  784 |   514 |
| **labyrinth**   |   1ms |    73 |    90.7 |  11 |   355 |  17.3 |   0 |   88 |     3 |
| **ssca2**       |  32ms | 10002 |     4.8 |   3 |     9 |   0.0 |   0 |    0 |     0 |
| **vacation-high**| 13ms |  4096 |    14.7 |   2 |    23 |   5.0 |   0 |    8 |     3 |
| **vacation-low** | 12ms |  4099 |    12.0 |   2 |    15 |   4.5 |   0 |    6 |     3 |
| **yada**        |   8ms |  1366 |    66.3 |   3 |   286 |   8.5 |   0 |  204 |   682 |

*intruder uses serialize locks (not TM), so no TM stats.

Key observations:
- **ssca2**: 100% read-only (0 writes) — graph traversal is inherently read-only
- **genome**: Only 10 large TXes, 46080 avg reads each — very read-heavy (hashing/pattern matching)
- **bayes**: 2016 avg reads, only 3.9 writes — very read-heavy Bayesian network learning
- **kmeans**: ~6200 reads + ~670 writes per TX — balanced, moderate contention (500+ aborts)
- **yada**: 66 avg reads, 8.5 avg writes, 682 aborts — moderate write contention (triangulation conflicts)
- All benchmarks have 0 user-level aborts except kmeans (reports via internal counter)

### Key decisions
- TX functions using `tm_serialize_lock/unlock` must NOT have `TX` attribute — the LLVM pass's instrumentation conflicts with the manual serialize lock. Use plain functions instead.
- TX functions using `tm_serialize_lock/unlock` on large writes also need `tm_allow_opaque` (for genome's dedup step that writes ~500K words per TX).
- Heap allocations inside a TX — even inside an opaque body — are intercepted by TinySTM's `operator new` hook → `bad_alloc`/`length_error`. Move all allocs before TX boundary.
- Replace `std::set<YadaEdge>` border_edges with `std::vector<YadaEdge>` + sort/dedup (cancel pairs). Set keeps duplicates, canceling requires count==1.
- `stop_workers` must be explicitly set or workers spin forever on empty heap. Use `empty_count >= 3` heuristic.

### All files modified
- `benchmarks/plugin/STAMP/kmeans_bench.hpp`
- `benchmarks/plugin/STAMP/vacation_bench.hpp`
- `benchmarks/plugin/STAMP/genome_bench.hpp`
- `benchmarks/plugin/STAMP/intruder_bench.hpp`
- `benchmarks/plugin/STAMP/bayes_bench.hpp`
- `benchmarks/plugin/STAMP/yada_bench.hpp`
- `benchmarks/plugin/STAMP/stamp_common.hpp`
- `benchmarks/plugin/STAMP/STAMP.cpp`
- `benchmarks/scripts/profile_stamp.py`

### Input file compatibility with original STAMP (ccaominh/stamp)

The paper's Table IV specifies input file flags (`-i`), but our implementations generate data inline:

| Benchmark | Paper param | Original input | Our approach | Compatible? |
|-----------|-------------|----------------|--------------|-------------|
| **yada** | `-a20 -i 633.2` | Triangle `.node`/`.ele` files (1264 elems) | Synthetic 10×10 grid + jitter (162 elems) | ✅ Code already reads `.node`/`.ele`; but input files not downloaded; synthetic is **8× smaller** |
| **yada+** | `-a10 -i ttimeu10000.2` | 19998 elements | Same 162-element synthetic | Same code path; synthetic is **123× smaller** |
| **labyrinth** | `-i random-x32-y32-z3-n64` | Text maze file | Inline generation via `-x -y -z -n` | ✅ Same maze dimensions; generation matches original algorithm |
| **kmeans** | `-i random-n2048-d16-c16` | Gzipped point data | Inline PRNG generation via `-m -n` | ✅ Same sizes; different PRNG (seed 42); distribution similar |

**Key insight**: Our synthetic yada mesh (162 elements) is far smaller than the paper's reference inputs (1264-19998 elements). The paper uses `-i` for input files; our synthetic generation uses `-j` for jitter and generates a 10×10 grid. To match paper workload sizes, we'd need to download the original input files from `https://github.com/ccaominh/stamp` or increase the synthetic grid size.

### Plugin known issues confirmed by profiling

- **genome**: TIMEOUT at 4t — known segfault at multi-thread (concurrent phase mismatch in genome_dedup/genome_match)
- **yada**: TIMEOUT at 4t — hangs after mesh generation with synthetic data (pre-existing, may be related to work heap size with 162 elements vs YADA_MAX_ELEMENTS)
- **intruder**: Crash with `[TM-REGION] mmap` — tiny STM tries to allocate 16384 MB region, likely failing on this system
- **kmeans**: Shows 0ms elapsed — converges instantly with threshold 0.05 and 2048 points (delta drops below threshold in 1 iteration)

## Previous Session (2026-06-07) — Serialize lock leak fix + genome/bayes/intruder lock restoration

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

## Why the LLVM TM pass cannot instrument STL containers

The plugin's LLVM pass instruments memory accesses (loads/stores) by:
1. Identifying which allocations are "TM-tracked" (from `tm_calloc`/`tm_malloc`, or inside `struct TM` globals)
2. Tracing pointer provenance back to those tracked allocations
3. Replacing tracked loads/stores with `tm_read_*`/`tm_write_*` runtime calls

**`std::vector` inside a `struct TM` fails because:**

The vector stores its elements in a heap-allocated buffer pointed to by internal fields (`_M_start`/`_M_finish` in libc++). The LLVM pass CAN instrument writes to those pointer fields (they're direct struct fields of the `TM`-annotated object). But when code writes through them — e.g., `*end_ = val` inside `push_back()` — the pass tries to trace the base pointer (`end_`) back to a TM-tracked allocation:

1. `end_` was loaded from the vector's internal field (a non-instrumented load of a thread-local or stack address)
2. The loaded value is a heap pointer from `::operator new()` (inside the STL allocator)
3. The pass cannot see that `::operator new()` returned TM-tracked memory — it only knows about `tm_calloc`
4. So the store through `*end_` is NOT instrumented

**Consequence**: Multiple threads inside `TX` functions can concurrently modify the same vector element without the TM runtime detecting the conflict. This causes silent data corruption — exactly the yada work-heap crash.

**`std::set`/`std::map` inside a `struct TM` fails because:**

These containers use internal red-black trees with opaque C library functions (libstdc++ `_Rb_tree_insert_and_rebalance`). The LLVM pass never sees the IR for these functions, so all pointer manipulations inside them are invisible. Writes to the tree structure are never instrumented.

**Workarounds (both demonstrated in the codebase):**

| Approach | When to use | Example |
|----------|-------------|---------|
| Flat `tm_calloc` arrays + read/write macros | Fixed-size or max-size buffers | `yada_bench.hpp` — `YADA_READ`/`YADA_WRITE` macros on `tm_calloc`'d arrays |
| Custom `SimpleVec` with raw pointer fields | Dynamic resizing needed | `tests/plugin/test_simple_vector.cpp` — `SimpleVec<T>` with `T* begin_, end_, cap_` |

Both work because the pointer provenance IS traceable: `tm_calloc` returns a known-TM base, and the custom struct's pointer fields are directly inside a `struct TM`, so the pass instruments both the field writes AND the data writes through them.

**Existing reproducer tests:**
- `tests/plugin/test_vector_realloc.cpp` — Shared `TM std::vector<int64_t>` with concurrent read/write (data corruption)
- `tests/plugin/test_realloc_crash.cpp` — `TM std::vector<AtomicPart>` with inner vectors (bad_alloc from STMbench7)
- `tests/plugin/test_stl_containers.cpp` — `TM std::vector<int>` and `TM std::unordered_map` (hangs/crashes at multi-thread)
- `tests/plugin/test_stl_vector_race.cpp` — Yada-style work-heap pattern: shared vector + concurrent push/pop (this session)
- `tests/plugin/test_simple_vector.cpp` — Workaround with custom `SimpleVec` (raw TM-tracked pointer fields)

## Bugs (current status)

### Fixed this session

| Bug | Fix |
|-----|-----|
| Serialize lock leaked across `siglongjmp` in TinySTM abort | Added thread-local counter + `tm_serialize_unlock_all()` in `abort_tx()` for WBCTL/WT backends |
| Genome/bayes/intruder crashes at multi-thread (STL corruption) | Restored `tm_serialize_lock/unlock` in all three benchmarks now that leak is fixed |
| Yada plugin crash at ≥2t (all backends) | Merged two TX functions into one TOCTOU-safe refine; replaced heap with flat TM-safe arrays |
| **Bayes 4-thread hang** | `compute_density_ll` not marked `TX`+`tm_allow_opaque` — pass instrumentation conflicted with serialize lock. **New**: added unlock before return. **Result**: 236ms at 4t |
| **Intruder crash with TinySTM** | `TX` attribute on `get_packet`/`process_decoder`/`get_complete` conflicted with manual `tm_serialize_lock/unlock`. Removed `TX` from all three (plain functions using serialize lock only) |
| **Yada timeout (all backends)** | 3 bugs: missing `stop_workers` (added empty_count≥3); wrong border-edge dedup (`std::set` kept interior edges → moved to THREAD + sort/dedup by cancel-pairs); `border_count≥3` limit removed. **Result**: 8ms at 4t |
| Kmeans/C++/Rust CLI flag incompatibility | Per-impl CLI_ARGS dict in profile_stamp.py handles -m/-n (plugin), -k/-d/-n (C++), -c/-d/-n (Rust) |
| Thread flag variation (-p vs -t) per benchmark | Per-benchmark THREAD_FLAGS dict in profile_stamp.py |
| C++ Makefile backend case mismatch | BACKEND_EXPLI map (tinystm→TINYSTM) in profile_stamp.py |

### Unfixed
| Genome WBCTL TIMEOUT at ≥4t | plugin | Concurrent phase mismatch between `genome_dedup`/`genome_match` (no barrier) — **confirmed by profiling** |
| Vacation WBCTL (plugin) 11-14x slower than C++ | plugin | 1000× more aborts (10816 vs 13) — investigate contention source |
| Vacation segfault at ≥28t during cleanup | plugin | Pre-existing, sporadic |
| stmbench7 WBCTL vector realloc crash | plugin | Pre-existing, fundamental STL incompatibility |
| stmbench7 NOREC cleanup hang ≥2t | plugin | Pre-existing |
| stmbench7 uninstrumented build failure | plugin | Missing `tm_treap_map.hpp` |
| TL2 build failure | plugin | Missing `tl2/tl2.hpp` |
| **Plugin genome 87x slower than C++** | plugin | Each large TX (46080 avg reads) has high TM overhead — investigate if `tm_allow_opaque` can reduce TX size |
| **Plugin kmeans-high 1442ms** | plugin | Moderate contention (569 aborts) — investigate convergence vs paper params |

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
- `benchmarks/scripts/profile_stamp.py` — STAMP profiling harness (paper params, per-impl CLI)
- `benchmarks/stamp_characterization.csv` — OCR'd Table VI (20 rows, 13 cols)
- `benchmarks/plugin/STAMP/yada_bench.hpp` — Fixed: single-TX refine + flat TM arrays
- `benchmarks/plugin/STAMP/bayes_bench.hpp` — Fixed: compute_density_ll TX+tm_allow_opaque+serialize_lock (fixed 4t hang)
- `benchmarks/plugin/STAMP/intruder_bench.hpp` — Fixed: removed TX from serialize-lock functions (fixed crash)
- `backends/tm_impl/tiny_stm/TinySTM_runtime.cpp` — Added: g_tm_min_read_set, g_tm_min_write_set, g_tm_total_commit_reads, g_tm_total_commit_writes, g_tm_commit_count for detailed TM_STATS
- `patch/profile/0001-add-tm-metrics.patch` — Cumulative patch: TinySTM metrics + bayes flag + compute_density_ll fix
- `patch/profile/run_profiling.py` — Detailed profiling with TM_STATS parsing + Table VI comparison
- `patch/profile/compare_table_vi.py` — Standalone CSV-to-Table-VI comparison
- `patch/profile/README.md` — Patch documentation and usage
- `tests/plugin/test_stl_vector_race.cpp` — STL vector race reproducer (shared work-heap pattern)
- `tests/plugin/test_simple_vector.cpp` — Workaround: custom SimpleVec with TM-tracked pointer fields
- `tests/plugin/test_vector_realloc.cpp` — Shared std::vector concurrent read/write reproducer
- `docs/AGENTS.md` — Session notes with profiling results and input compatibility analysis
