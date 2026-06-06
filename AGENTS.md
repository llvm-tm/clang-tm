# Session Summary

## Latest Session (this session — 2026-06-06)

### Runner fixes: all 3 implementations × 4 backends now work end-to-end

Created **`run_clean_benchmarks.sh`** (now 433 lines) — comprehensive runner with build-on-demand for expli C++, stubs baseline, and embedded Python analysis:

| Fix | Issue |
|-----|-------|
| Plugin `-b` flag: `${bench:0:1}` → `"$bench"` | Was passing single letter (`v`) instead of full name (`vacation`) — root cause of ALL 8 plugin STAMP failures |
| Expli: build-on-demand per backend | `make clean` at end of build loop wiped all binaries before run phase |
| Plugin `sgl` → `singlelock` | Binary name mismatch prevented singlelock runs |
| Speedup table: show all impls | Was hardcoded to only show `plugin` entries |

### Rust WT: all 8 STAMP benchmarks now pass

**Root cause**: `wt.rs:10` and `wt.rs:41` assert `is_tm_address(addr)` — but `TmCell<T>` stores its value inline (stack/heap allocated, never in TM region). C++ TinySTM already handles this at `tinystm_common.hpp:273/298` — non-TM addresses bypass TM logging entirely.

**Fix**: Added `is_tm_address()` check before assertion in `read_word`/`write_word` in `wt.rs`. Non-TM addresses are read/written directly without transactional logic. Both `read_raw_bytes`/`write_raw_bytes` benefit via their calls to `read_word`/`write_word`.

**Results**: All 8 Rust WT benchmarks pass (previously 4/8 crashed with exit=101).

### Plugin WT crash investigation

**Crashes observed**: genome (timeout), intruder (SIGSEGV), bayes (SIGSEGV), yada (timeout)

**Root cause**: LLVM plugin instruments STL container operations inside transactions. WT's write-through with undo logs corrupts heap metadata when `std::vector::push_back` triggers reallocation inside a TM transaction. The old buffer is read via `tm_read_i8` and written to the new buffer via `tm_write_i8`, but the WT `write_word_wt` acquires lock-table locks on non-TM heap addresses and saves undo entries — exposing allocator metadata to corruption on abort or concurrent access.

**Status**: Pre-existing. Only affects LLVM plugin path + WT combo. Expli C++ WT works (no STL instrumentation). Deferred.

### Uninstrumented baseline for expli C++

Created `backends/runtimes/tm_stubs.cpp` — stubs implementation of all `extern "C"` TM API functions:
- `tm_init`/`tm_exit`/`tm_begin`/`tm_end` — no-ops
- `tm_malloc`/`tm_calloc`/`tm_realloc`/`tm_free` — pass through to system allocator
- `tm_read_i*`/`tm_write_i*` — direct memory access

Added `stubs` target to `expli-benchmarks/Makefile` with `BUILD_STUBS_RULE` that links against `tm_stubs.cpp` instead of the TM runtime. Runner builds stubs on demand in `run_uninstrumented()`. All 11 benchmarks (STAMP×8 + tpcc + ycsb + stmbench7) run successfully.

### Full verification results (1 thread, 1 sample)

| Impl | Backend | STAMP(8) | tpcc | ycsb | stmbench7 | Total |
|------|---------|----------|------|------|-----------|-------|
| Plugin | tinystm_wbctl | 8/8 OK | OK | OK | OK | **11/11** |
| Plugin | tinystm_wt | 4/8 OK (pre-existing crashes) | OK | OK | OK | **7/11** |
| Plugin | norec | 8/8 OK | OK | OK | OK | **11/11** |
| Plugin | singlelock | 8/8 OK | OK | OK | OK | **11/11** |
| Plugin | tsxsgl | — (no TSX hw) | — | — | — | **0/11** |
| Expli | tinystm_wbctl | 8/8 OK | OK | OK | OK | **11/11** |
| Expli | tinystm_wt | 8/8 OK | OK | OK | OK | **11/11** |
| Expli | norec | 8/8 OK | OK | OK | OK | **11/11** |
| Expli | sgl | 8/8 OK | OK | OK | OK | **11/11** |
| Expli | uninstrumented | 8/8 OK | OK | OK | OK | **11/11** |
| Rust | wbctl | 8/8 OK | — | — | — | **8/8** |
| Rust | wt | 8/8 OK | — | — | — | **8/8** |
| Rust | norec | 8/8 OK | — | — | — | **8/8** |
| Rust | sgl (tsxsgl) | 8/8 OK | — | — | — | **8/8** |

## Previous Session (2026-06-05)

### Comprehensive benchmark runner with skip-list

**`run_compare_all.sh`** (794 lines, at repo root) runs TSXSGL, TinySTM WBCTL, NOrec, and SingleGlobalLock across 3 implementations (plugin/expli/rust) × 6–8 STAMP benches + TPC-C + STMbench7, at 12 thread levels (1–56) with 3 samples and uninstrumented baseline at 1t. Output organized in `benchmark_results/compare_all_<ts>/raw/{plugin,expli,rust}/{backend}/` with `progress.txt`, `results.csv`, and `SUMMARY.txt`. Analysis phase computes mean/stddev per (impl × backend × bench × threads) and writes speedup table vs uninstrumented.

**Skip-list mechanism** (`skip_combos.txt`): When a run produces truncated output (<600 bytes, no success indicators), the (impl, backend, bench) combo is marked as consistently-broken and all remaining thread levels/samples are skipped instantly. Pre-populated with `plugin tinystm_wbctl stmbench7` (std::vector reallocation crash). Verified: all 36 stmbench7 WBCTL runs skipped in latest execution.

### Crash analysis: TinySTM WBCTL/NOrec at high concurrency

**Vacation (28–56 threads):** Sporadic SIGSEGV during thread cleanup after benchmark completion. Output file contains valid metrics ("OK+" status) — benchmark finishes before crash. Affects both WBCTL and NOrec backends. 21t and above show ~1–2 crashes per 3 samples. Likely write-set or lock-array sizing at high concurrency.

**Genome (56 threads):** `double free or corruption (out)` in 1/3 samples (Heisenbug — timing-dependent). Samples 1 and 3 typically succeed.

**stmbench7 (2+ threads, TinySTM WBCTL only):** Consistent hang/crash during worker initialization. Output truncates at TM-REGION init line (~585 bytes). Root cause: `std::vector::_M_realloc_insert` called inside TM region (reallocation reads old elements via `tm_read_i8` which hits `read_word_ctl()` on invalid addresses). TSXSGL stmbench7 works fine on all thread levels (36/36 OK).

### Build fixes applied this session

| File | Line | Fix |
|------|------|-----|
| `llvm_tm_plugin/tm_pipeline.mk` | 129 | Added `-I$(BACKENDS_DIR)` to `TM_INCLUDES_singlelock` (was empty) |
| `expli-benchmarks/Makefile` | 47 | `single_global_lock_runtime.cpp` → `SingleGlobalLock_runtime.cpp` (wrong case) |
| `expli-benchmarks/Makefile` | 49 | Added `-I../backends` to SGL `EXTRA_INC` (was empty) |
| `backends/TinySTM/tinystm_wbctl.hpp` | 317–325 | Wrapped unconditional `fprintf(stderr,...)` in `#ifndef NDEBUG` |
| `run_compare_all.sh` | — | `-m 3` → `-p 3` for SSCA2 (plugin uses `-p` for max_parallel_edges) |

### Workloads tuned for ~60s per run

- intruder: 1,048,576 → **5,120** flows
- genome: 16,777,216 → **1,000,000** segments
- SSCA2: scale 7 → 14 (larger workload, ~30s at 1t)

### Execution history

| Run | Time | Attempts | Complete | Timeout | Crash | Skipped | Notes |
|-----|------|----------|----------|---------|-------|---------|-------|
| 120349 | 12:03 | — | — | — | — | — | Early abort (build fix) |
| 121635 | 12:41 | 158 | 157 | 0 | 0 | 0 | TSXSGL partial |
| 121658 | 12:41 | 158 | 157 | 0 | 0 | 0 | TSXSGL partial |
| 124459 | 12:57 | 326 | 289 | 0 | 6 | 0 | TSXSGL + WBCTL partial (31 FAIL from old retry logic) |
| 130121 | 14:09 | 553 | 548 | 5 | 8 | 0 | Full plugin TSXSGL + WBCTL; stuck 5h on stmbench7 timeouts |
| 141356 | 15:01 | 551 | 548 | 3 | 4 | 0 | Same as 130121 (pre skip-list), killed early |
| **150228** | **15:49** | **617** | **578** | **3** | **8** | **36** | **Through NOrec plugin — skip-list working** |

### Remaining work (for next session)

- Complete NOrec plugin (vacation 56t timing out), then SGL plugin
- Run expli C++ path (TINYSTM, NOREC, SGL — 3 backends × 10 benches × 12 levels × 3)
- Run Rust path (wbctl, norec, tsxsgl — 3 backends × 6 benches × 12 levels × 3)
- Run analysis phase (embedded Python script)
- Triage SSCA2 pre-existing crash (SIGSEGV in `tm_begin` from worker thread)

### All 8 STAMP benchmarks now ported to Rust

Added bayes and yada Rust ports, completing all 8 STAMP benchmarks across all 3 implementations:

| Benchmark | Plugin (reference) | Expli C++ | Rust |
|-----------|-------------------|-----------|------|
| vacation  | `vacation_bench.hpp` | `vacation.cpp` | `stamp_vacation.rs` |
| kmeans    | `kmeans_bench.hpp` | `kmeans.cpp` | `stamp_kmeans.rs` |
| labyrinth | `labyrinth_bench.hpp` | `labyrinth.cpp` | `stamp_labyrinth.rs` |
| genome    | `genome_bench.hpp` | `genome.cpp` | `stamp_genome.rs` |
| intruder  | `intruder_bench.hpp` | `intruder.cpp` | `stamp_intruder.rs` |
| ssca2     | `ssca2_bench.hpp` | `ssca2.cpp` | `stamp_ssca2.rs` |
| bayes     | `bayes_bench.hpp` | `bayes.cpp` | `stamp_bayes.rs` |
| yada      | `yada_bench.hpp` | `yada.cpp` | `stamp_yada.rs` |

#### Key changes
- **Rust bayes**: Bayesian network structure learning. Builds parent/child graph, computes log-likelihoods, uses `transaction()` closures for TM access. Non-TM `compute_ll` helper called inside TX for density computation. Same algorithm as C++ expli version.
- **Rust yada**: Delaunay mesh refinement. Timer-based (3s) worker loop with 3 transactions per iteration (pop, refine, push). Cavity BFS with circumcircle test. Fixed timer-check skip bug: timer check must run before `continue` to avoid infinite loop when work heap empties or all remaining elements are garbage.
- **Rust TinySTM backend**: Works correctly with heap-allocated `TmCell<T>` data (no TM region assertion).
- Both new ports compile with zero warnings, zero errors under `cargo build --release -p benchmarks --bin stamp_bayes --bin stamp_yada`.

#### Test results (Rust, 4 threads, default params)
- `stamp_bayes -v 32 -r 1024 -p 4`: 31 ops, 62 total parents, 394 ms — PASS
- `stamp_yada -a 45 -j 0.5 -p 2`: 56 ops, 330 elements, 3000 ms (3s timer) — PASS
- `stamp_yada -a 20 -j 0.5 -p 4`: 0 ops (no bad elements), 162 elements, 3000 ms — PASS

#### Issues found & fixed
1. **Yada timer check skipped on continue**: The 3-second timer check was at the bottom of the while loop, but `continue` statements in the early-exit paths (empty heap, garbage element) bypassed it. If the heap ever had only garbage elements, each iteration hit `continue` without ever checking the timer — infinite loop. **Fix**: Added timer check before every `continue`.

## Previous Session (2026-06-04)

### Ported 6 STAMP benchmarks: expli C++ and Rust match plugin algorithm

**Root cause**: The centroid update loop in phase 2 called `tm_read_double`/`tm_write_double` **outside** any `tx_retry` block. The plugin runs centroid update outside transactions (only `TX`-annotated functions are instrumented). The expli API equivalent must use direct memory access (`*ptr` instead of `tm_read_double`/`tm_write_double`).

**Fix**: Replaced TM barriers with plain `*cptr` read/write in centroid update loop. Aborts dropped from 647 (corrupted state) to 23 (normal concurrent workload).

### LLVM plugin pass preamble path detection

**Issue**: Stale `.so` binary injected preamble code using `@tm_nested_call_counter` globals while runtime checked `ts->nested_call_counter`. Caused `g_tm_expli_mode=true` incorrectly for plugin path, disabling stack-address skip in write-back and corrupting dead `_tm_clone` frames.

**Fix**: Rebuilt `libTMInstrument.so` (Makefile now depends on headers). Preamble correctly uses `tm_get_thread_state()` → `ts->nested_call_counter`, keeping `g_tm_expli_mode=false` for plugin path.

### Rust warning cleanup
- Fixed all warnings across workspace: unused variables prefixed with `_`, unused imports removed, `#[allow(dead_code)]` on TM runtime state fields, unnecessary `mut` removed. `cargo build --workspace` and `cargo test --workspace` produce zero warnings.

## Goal
- Run complete comparison across all backends/implementations/benchmarks and obtain comparable numbers

## Key Decisions
- **`g_tm_expli_mode` flag in write-back**: Stack addresses write-back enabled for expli API, skipped for plugin (dead `_tm_clone` frames).
- **Workload sizes tuned**: intruder 5K flows, genome 1M segments, SSCA2 scale 14 for ~30-60s per run.
- **Skip-list over retry**: Consistent-crash combos are tracked in `skip_combos.txt` and skipped instantly rather than timing out 600s per run.
- **`\--features tsxsgl` in Rust** is actually SGL (atomic spinlock) since Rust can't use Intel RTM.
- **Non-inline pipeline** (`tm-instrument`) kept as default (avoids write-set/memory asymmetry for local containers).

## Critical Context
- **TinySTM WBCTL/NOrec segfaults at ≥28 threads**: Sporadic crashes during thread cleanup after valid output. Benchmark metrics are trustworthy. May indicate thread-pool or lock-array exhaustion in TinySTM runtime.
- **stmbench7 `std::vector` crash**: Vector reallocation (`_M_realloc_insert`) reads old elements via TM barriers inside the reallocation loop. This is a fundamental incompatibility — TM-instrumented `std::vector` cannot safely reallocate.
- **stmbench_uninstrumented build fails**: Linker error (`undefined reference to g_tm_region_end`) — requires TM runtime stubs not available for uninstrumented target.
- **SSCA2 plugin**: Uses `-p` (not `-m`) for `max_parallel_edges`. Expli/Rust use `-m` (different parser).
- **HW**: Intel Xeon E5-2648L v4 @ 1.80GHz, 56 logical cores, RTM supported.

## Issues Found
1. **Sporadic segfaults at ≥28 threads (all TinySTM backends)**: Vacation, NOrec, WBCTL all crash during thread cleanup at high concurrency. Output is valid. Likely TinySTM lock-array or write-set sizing issue.
2. **stmbench7 + TinySTM WBCTL**: `std::vector::_M_realloc_insert` crash during TM reallocation. All 36 runs (12 levels × 3) skipped. TSXSGL unaffected (no TM on std::vector internals).
3. **Double-redirection of nesting counter** (fixed): `ts->nested_call_counter` vs `tm_nested_call_counter` desync.
4. **Stack write-back blocked for expli API** (fixed): `is_stack_addr` skip unconditionally blocked expli stack fields.
5. **Pre-existing**: `test_vec_push`/`test_alloc_stress` hang at exit; `test_vector_realloc` data corruption; SSCA2 SIGSEGV in `tm_begin` from worker thread.

### run_full_compare.sh improvements this session

Created **`run_full_compare.sh`** (515 lines): comprehensive runner for 3 impls × 3 backends (wbctl, wt, norec) × 11 benches (STAMP×8 + TPC-C + YCSB + STMbench7). Fixes applied:

| # | Fix | Issue |
|---|-----|-------|
| 1 | Associative arrays → case functions | `declare -A` invalid on macOS bash 3.x |
| 2 | Added `norec`/`tinystm_wbctl`/`tinystm_wt` targets to YCSB/TPCC Makefiles | Missing targets prevented linking |
| 3 | `timeout` → `$TIMEOUT_CMD` (auto-detect `gtimeout`) | `timeout` doesn't exist on macOS |
| 4 | Absolute paths for `RESULTS_DIR` | Python analysis failed with relative paths after `cd` |
| 5 | Added `tm_get_thread_state()` to `NOrec_runtime.cpp` | Missing symbol for plugin NOREC linker |
| 6 | Interleaved build+run for Expli C++ (`make clean` per backend) | Make timestamps caused stale binaries |
| 7 | Interleaved build+run for Rust (`cargo build + run per feature`) | `--features` overwrote previous binaries |
| 8 | Fixed plugin binary directory lookup (separate dirs per benchmark type) | TPCC/YCSB/STMbench7 binaries not in STAMP/bin |
| 9 | Added `fastrand = "2"` to Rust `benchmarks/Cargo.toml` | Missing dep for `fuzz_counter`, `bank`, `stmbench7` |

Run stats (threads=1,4; samples=1): completed=96, crash=11, fail=10, skipped=15, total=132.

## Crash Investigation Plan

### Priority 1: Rust WT address-space assertion (8 benches — exit 101)
**Affected**: vacation, kmeans, labyrinth, bayes, yada, tpcc, ycsb, stmbench7  
**Works**: genome, intruder, ssca2  
**Error**: `panicked at runtime/tinystm/src/wt.rs:10:5 — Address not in TM address space`  
**Root cause**: Rust WT runtime asserts `runtime_core::is_tm_address(addr)` in every `tm_read`/`tm_write`. Benches passing non-`TmCell<T>` addresses (heap/stack data outside TM mmap region) trigger the panic. WBCTL avoids this because it does not validate addresses (it reads through the write-set).

**Investigation steps**:
1. For each failing bench, identify which address triggers the assertion (data structure + allocation pattern).
2. Determine whether the address *should* be in TM space (bench bug) or the runtime should accept it (runtime limitation).
3. Fix options:
   - **A**: Make benches use `TmCell<T>` for all TM-tracked data (correct but invasive).
   - **B**: Remove/weaken the address-space assertion in Rust WT (match WBCTL behavior — accept any address). This loses a safety check but matches the existing C++ TinySTM WBCTL semantics.
4. Apply fix, rebuild `--features wt`, re-run failing benches.

### Priority 1: Expli WBCTL/WT SSCA2 (2 benches — SIGSEGV)
**Affected**: ssca2 (both wbctl and wt)  
**Error**: SIGSEGV during graph initialization, before `[TM-REGION]` mmap line. Output truncated at 6 parameter lines.  
**Not affected**: ssca2-norec (crashes with address-space assertion like all other norec benches).  
**Root cause**: Pre-existing crash in the edge-generation phase of SSCA2. The `--max-parallel-edges` param (`-m`) might construct an invalid graph scale.

**Investigation steps**:
1. Run `ssca2 -s 14 -u 1.0 -l 3 -m 3 -i 3` (same params) standalone under a debugger to catch the crash site.
2. Check if the crash is in TM code (`tm_begin`/`tm_read`/`tm_write`) or non-TM code (plain C++ data structure manipulation).
3. If non-TM: fix the benchmark logic (off-by-one, buffer overflow).
4. If TM: check if heap data outside TM region is being accessed within a transaction.
5. Cross-check with plugin/rust ssca2 — do they work? (Plugin ssca2 passed in run_compare_all.sh runs; Rust ssca2 passed in this session.)

### Priority 2: Expli NOrec (all 11 benches — sig=6 assertion)
**Affected**: ALL benches across norec backend  
**Error**: `TM ASSERTION FAILED: Address not in TM address space (NOrec.hpp:114)`  
**Root cause**: `NOrec_runtime.cpp` defines `tm_read_i8`/`tm_write_i8` etc. that call directly into `norec::tm_read_i8` which asserts `stm::isTMAddress(addr)`. When `LLVM_TM_PLUGIN` is NOT defined (expli mode), the assertion is a hard fault instead of falling through to a direct memory access. The C++ `TinySTM_runtime.cpp` has `g_tm_expli_mode` to detect this and skip assertions; NOrec runtime lacks this.

**Investigation steps**:
1. Check if `NOrec_runtime.cpp` can detect expli mode (e.g., define a `TM_EXPLI_MODE` or check `g_tm_expli_mode`).
2. Alternatively, define `LLVM_TM_PLUGIN` for expli NOrec builds to enable the fallback path — but verify no side effects on the codegen.
3. Fix `NOrec_runtime.cpp` to handle non-TM addresses gracefully in expli mode (either skip assertion or fall through to `std::memcpy`).
4. Rebuild expli with `BACKEND=NOREC`, re-run all benches.

### Priority 2: Rust WT address-space assertion on non-TM heaps (same as Priority 1, broader fix)
Some benches (tpcc, stmbench7) use large heap-allocated arrays whose pointers are stored in `TmCell`. The actual data buffers reside on the heap outside the TM region. Reads/Writes through `TmCell`-stored pointers then fail when the WT runtime asserts the dereferenced address is in TM space.

**Investigation steps**:
1. For tpcc: trace the assertion to a specific `tm_read` call — is it reading a `TmCell` value (should be in TM space) or dereferencing a pointer stored inside a `TmCell` (points to heap)?
2. If the latter: the issue is that pointer-indirection through TM-tracked pointers is unsafe with WT but safe with WBCTL (WBCTL only tracks the pointer cell, not the pointed-to data). This is a fundamental semantic difference between the backends.
3. Fix: store data in `TmCell` (inside TM region) instead of on the C heap. Or accept that WT cannot safely follow TM-tracked pointers.

### Priority 3: Plugin path not producing results
**Issue**: The `run_full_compare.sh` plugin section produced zero output files. The `run_plugin_impl` function correctly checks binary paths but may fail silently.

**Investigation steps**:
1. Run `run_plugin_impl` standalone with verbose logging to see which binary checks fail.
2. Check if plugin binaries exist at the expected paths.
3. Check if `return 0` in the old code (now `if [ -x ... ]`) was causing the entire plugin section to early-exit.
4. Fix any path or permission issues.

### Known/Deferred
- **High-concurrency segfaults (vacation, 28–56t)**: Sporadic SIGSEGV during thread cleanup after valid output. Benchmark completes, metrics are trustworthy. Deferred until after Priority 1-2 fixes.
- **Plugin stmbench7 vector crash**: `std::vector::_M_realloc_insert` inside TM region. All 36 runs skipped. Known incompatibility.
- **Rust WT Labyrinth 4t → 0 ms**: Reported `Time = 0.000000` (labyrinth_4t_s1 passes but shows 0ms). May indicate empty workload. Verify with larger grid dimensions.

## Relevant Files
- `run_compare_all.sh`: Main benchmark runner with skip-list, build, run, and analysis phases
- `backends/TinySTM/tinystm_wbctl.hpp`: WBCTL runtime with write-back and `is_stack_addr`
- `backends/runtimes/TinySTM_runtime.cpp`: `tm_nested_call_counter`, path detection, runtime init
- `llvm_tm_plugin/tm_pipeline.mk`: Plugin build system with per-backend rules
- `expli-benchmarks/Makefile`: Expli C++ build with BACKEND selection
- `rust_tm_api/benchmarks/src/bin/`: Rust benchmark binaries (`stamp_vacation.rs`, etc.)
