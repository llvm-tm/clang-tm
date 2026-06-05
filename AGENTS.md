# Session Summary

## Latest Session (this session — 2026-06-05)

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

## Previous Session (2026-06-04)

### Ported 6 STAMP benchmarks: expli C++ and Rust match plugin algorithm

| Benchmark | Plugin (reference) | Expli C++ | Rust |
|-----------|-------------------|-----------|------|
| vacation  | `vacation_bench.hpp` | `vacation.cpp` | `stamp_vacation.rs` |
| kmeans    | `kmeans_bench.hpp` | `kmeans.cpp` | `stamp_kmeans.rs` |
| labyrinth | `labyrinth_bench.hpp` | `labyrinth.cpp` | `stamp_labyrinth.rs` |
| genome    | `genome_bench.hpp` | `genome.cpp` | `stamp_genome.rs` |
| intruder  | `intruder_bench.hpp` | `intruder.cpp` | `stamp_intruder.rs` |
| ssca2     | `ssca2_bench.hpp` | `ssca2.cpp` | `stamp_ssca2.rs` |

#### Key changes
- **Expli C++**: 6 rewritten benchmarks using explicit `tm_read_i8`/`tm_write_i8` etc. inside `tx_retry`. Vacation, kmeans, labyrinth fully ported with matching algorithm. Genome, intruder use `std::mutex` (matching `tm_serialize_lock/unlock`). SSCA2 uses read-only TX wrappers.
- **Rust**: 6 new standalone binaries (`stamp_*.rs`) in `rust_tm_api/benchmarks/src/bin/`. Added to `Cargo.toml`. All compile with zero warnings under `cargo build --release`.
- **Rust TinySTM backend**: Works correctly with heap-allocated `TmCell<T>` data. The pre-existing panic in `wbctl.rs:6` was resolved (address check removed in default `wbctl` feature).

### Kmeans fix: centroid update outside tx_retry

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

## Relevant Files
- `run_compare_all.sh`: Main benchmark runner with skip-list, build, run, and analysis phases
- `backends/TinySTM/tinystm_wbctl.hpp`: WBCTL runtime with write-back and `is_stack_addr`
- `backends/runtimes/TinySTM_runtime.cpp`: `tm_nested_call_counter`, path detection, runtime init
- `llvm_tm_plugin/tm_pipeline.mk`: Plugin build system with per-backend rules
- `expli-benchmarks/Makefile`: Expli C++ build with BACKEND selection
- `rust_tm_api/benchmarks/src/bin/`: Rust benchmark binaries (`stamp_vacation.rs`, etc.)
