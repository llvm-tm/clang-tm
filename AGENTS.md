# Session Summary

## Latest Session (this session — 2026-06-05)

### All 8 STAMP benchmarks + TPC-C ported to spec-compliant expli C++

#### What was done
- **8 STAMP benchmarks** now match the plugin spec (same algorithm, same RNG seeds, same output):
  - vacation: best-price query, 3 table types, flat TM arrays on TM region
  - kmeans: convergence check, cluster-biased points, accumulate/update phases, barrier sync
  - ssca2: clique-based graph generation + triangle counting on CSR
  - genome: gene+segments, mutex-dedup, hash-based overlap matching
  - intruder: flow reassembly with attack detection, mutex-protected queues
  - labyrinth: off-TM BFS + TM-protected path mark (with sigsetjmp retry loop)
  - bayes, yada: already ported (from stamp_common split to standalone files)
- **TPC-C**: full v5.11 spec port — 5 transactions (New-Order, Payment, Order-Status, Delivery, Stock-Level), remote warehouse (1%/15%), unique items per order, History rows, flat TM arrays via `tm_calloc`
- **STMbench7**: functional with 12/45 ops, reduced scale (200 CP, 800 AP)

#### Test results (all via `make run-stamp` and `make run-tpcc`)
| Benchmark | Result | Notable |
|-----------|--------|---------|
| vacation  | 1.3M txns/sec | 2 aborts |
| labyrinth | 64/64 routed | 2 aborts |
| kmeans    | 52 iters, converged | 108 aborts |
| genome    | 16320 unique | 0 aborts |
| intruder  | 1M processed | 0 aborts |
| ssca2     | 1.7M triangles | R/O, no writes |
| bayes     | 60 parents | 2369 aborts |
| yada      | 411 elements | 247 aborts |
| tpcc      | 399 ops/sec | 555 aborts, 3s run |
| stmbench7 | 231K ops/sec | 112K aborts, 5s run |

#### Key implementation patterns for expli C++
- **tx_run template**: `volatile bool done` loop with `sigsetjmp(tm_jmpbuf,0)` + `tm_nested_call_counter = 1` before `tm_begin()`. The nesting counter signals the runtime to actually start a TinySTM transaction.
- **Flat TM arrays**: `tm_calloc` for all TM-tracked data, `TM_READ_I8`/`TM_WRITE_I8` macros wrapping `tm_read_i8`/`tm_write_i8` from the runtime. Direct stores during initialization (no TM needed).
- **Off-TM computation**: BFS (labyrinth), Hash table building (genome), Triangle counting (ssca2), Clustering (kmeans) all done outside TM — only the final write-back or check-and-mark uses TM barriers.
- **Mutex for serial access**: genome dedup, intruder queues — use `std::mutex` matching plugin's `tm_serialize_lock`.

#### Build and run commands
```sh
make -C expli-benchmarks run-stamp BACKEND=TINYSTM    # all 8 STAMP
make -C expli-benchmarks run-tpcc BACKEND=TINYSTM      # TPC-C
make -C expli-benchmarks run-bench7 BACKEND=TINYSTM    # STMbench7
```

### Remaining work
- **Rust address-space crash**: ALL Rust benchmarks panic (`"Address not in TM address space"`). Rust `TmCell` wraps heap-allocated (Box/Vec) data; TinySTM enforces TM-region-only addresses. Either allocate TmCell data from the TM region or relax the TinySTM assertion.
- **Plugin bugs**: EigenBench produces 1-op TX (instrumentation miss); STMBench7 OOM at medium OO7 scale (500 CP, 100K AP).
- **STMbench7**: 33 of 45 operations still missing from expli version.
- **Rust STAMP ports**: Not started (blocked by address-space issue).

## Previous Session (2026-06-04)

### Fix: LLVM plugin pass preamble path detection (stale binary)

#### Root cause: Plugin binary out of sync with source
`injectTransactionBeginEnd()` in `tm_instrument_helpers.hpp` was updated to use `tm_get_thread_state()` + GEP (avoiding TLV relocations), but the `.so` was not rebuilt (Makefile only listed `TMInstrumentPass.cpp` as a dependency, not the header). The stale binary injected preamble code using `@tm_nested_call_counter` globals directly. Since the preamble incremented the `__thread` variable while the runtime path-detection in `tm_begin()` checked `ts->nested_call_counter` (via `TMThreadState`), `g_tm_expli_mode` was incorrectly set to `true` for the plugin path. This disabled the stack-address skip guard in the write-back phase, causing dead `_tm_clone` frame writes to corrupt the current stack, leading to SIGSEGV in the heap.

**Fix**: Rebuilt the plugin `.so` (`make BUILD_TYPE=DEBUG`). The preamble now correctly uses `tm_get_thread_state()` → `ts->nested_call_counter`, keeping `g_tm_expli_mode=false` for the plugin path. Write-back correctly skips stack addresses.

**Note**: With the preamble fix, the stmbench7 test now progresses past the initial stack corruption but hits a pre-existing crash in `tinystm::read_word_ctl()` during `std::vector<Document>::_M_realloc_insert`. This is the known `std::vector` reallocation issue: the `_tm_clone` of `__relocate_a_1` reads old-element bytes via TM barriers (`tm_read_i1`) while the vector is relocated inside a transaction. TM-friendly containers (`TMTreapMap`, plain arrays) are unaffected — `test_treap_tx`, `test_ll_alloc`, `test_local_containers`, `test_construct_tx_pattern`, `test_simple_vector` all PASS.

#### Test results
- **LLVM plugin**: `test_treap_tx` PASS, `test_ll_alloc` PASS, `test_simple_vector` PASS, `test_construct_tx_pattern` PASS, `test_local_containers` PASS
- **Pre-existing issues unchanged**: `stmbench7` crashes in `std::vector::_M_realloc_insert` during TM relocation; `test_vec_push` and `test_alloc_stress` hang at exit; `test_vector_realloc` data corruption from `std::vector` reallocation races

#### Key changes
- `llvm_tm_plugin/bin/libTMInstrument.so`: Rebuilt from updated source (headers now properly compiled in)
- `backends/runtimes/TinySTM_runtime.cpp`: Removed ad-hoc debug `fprintf` from `tm_begin()` (path-detection logic retained, prints removed)
- `backends/TinySTM/tinystm_wbctl.hpp`: Removed ad-hoc debug `WB[]` prints from write-back phase

### Rust warning cleanup
- Fixed all Rust compiler warnings across the workspace (runtime backends + benchmarks): unused variables prefixed with `_`, unused imports removed, `#[allow(dead_code)]` on struct fields that are part of TM runtime state, unnecessary `mut` removed.
- `cargo build --workspace` and `cargo test --workspace` now produce zero warnings, zero errors.

### Infrastructure analysis: LeftRight, Romulus, XTM with queue executor + addrspace
- **LeftRight**: The queue executor doesn't help — LeftRight's performance depends on wait-free reads that never synchronize with writers. Adding queue-based completion tracking serializes readers and defeats the purpose. The two-copy + EBR drain protocol doesn't map cleanly to the queue model. Recommendation: skip the two-copy scheme and just submit all mutations to the queue executor for sequential processing (traditional STM with single writer).
- **Romulus**: Best fit. The address space's `spec_alloc` is a natural match for Romulus's redo log entries. At commit time, the queue executor applies the log atomically to the main copy. The challenge is read consistency — Romulus needs to read the "main copy" during TX execution for read-before-write, and the queue executor's sync mechanism doesn't provide snapshot isolation by itself. The executor would need to track read versions. Most feasible next target.
- **XTM**: Page-level versioning in the 64 GB address space is elegant — 64 KB chunk headers (already exist) could store per-chunk version numbers, giving ~1M chunks (manageable). 4 KB pages would be ~16M entries (more metadata overhead). The unsolved problem is write detection: either hardware mprotect (expensive) or software write barriers on every store (same runtime overhead as existing STMs). The addrspace helps with layout but doesn't solve the fundamental instrumentation problem.

## Goal
- Fix the LLVM plugin pipeline crash (PC=0, SIGSEGV) in TinySTM-backed treap tests and unify the runtime path for both expli API and LLVM plugin

## Key Decisions
- **`g_tm_expli_mode` flag in write-back**: The write-back phase in commit() needs to write to stack addresses for the expli API (stack-allocated `expli::TM<T>` fields) but skip them for the plugin (dead `_tm_clone` frames). A `thread_local bool` set by path detection in `tm_begin()` is the cleanest way to distinguish.
- **`tm_nested_call_counter` defined in runtime**: Must be `__thread int32_t` defined exactly once in `TinySTM_runtime.cpp`. The header-only `extern` declaration in `tinystm_wbctl.hpp` is only for compilation units that can't include the runtime definition.
- **Do NOT modify `tm_nested_call_counter` in `tm_end()`**: The expli API manages this variable's lifecycle (sets to 0 after `tm_end()` returns). If the runtime overwrites it, the next `begin()` sees the wrong value.

## Critical Context
- **Nesting counter sync**: `TinySTM_runtime.cpp` uses `ts->nested_call_counter` (from `TMThreadState`) while the expli API uses `tm_nested_call_counter` (bare `__thread`). Prior to this fix, `tinystm::begin()` was never called for the expli API path.
- **Stack write-back required for expli API**: The expli API places `expli::TM<T>` fields on the stack (e.g., `Point p; p.x.write(10)`). Without write-back to stack addresses, committed values are never persisted to memory. The plugin path must skip stack write-back to avoid corrupting dead `_tm_clone` frames.
- **`is_stack_addr` detection**: Uses `pthread_get_stackaddr_np()` + `pthread_get_stacksize_np()` to compute current thread's stack bounds. Cached per-thread via `thread_local StackBounds` struct to avoid repeated syscalls.
- **Lock acquisition/release skips stack addresses unconditionally**: Stack is per-thread, so no inter-thread locking is needed. The `is_locked_by()` check in the release loop already skips un-locked stack addresses.

## Relevant Files
- `backends/runtimes/TinySTM_runtime.cpp`: `tm_nested_call_counter` definition, `g_tm_expli_mode`, path detection, nesting counter sync
- `backends/TinySTM/tinystm_wbctl.hpp`: `g_tm_expli_mode` declaration, guarded `is_stack_addr` in write-back; `is_stack_addr()`, `get_stack_bounds()` helper

## Issues Found
1. **Double-redirection of nesting counter in `TinySTM_runtime.cpp`**: Originally used `__thread int32_t tm_nested_call_counter` directly, but a prior refactor changed to `ts->nested_call_counter` (from `TMThreadState`), breaking the expli API path. Now fixed with bidirectional sync in `tm_begin()`.
2. **Stack write-back blocked for expli API**: `is_stack_addr(addr) continue;` in commit's write-back phase skipped stack addresses unconditionally, preventing expli API `expli::TM<T>` fields from being persisted. Fixed with `g_tm_expli_mode` flag.
3. **Pre-existing issues**: `test_vec_push` / `test_alloc_stress` hang at exit (TinySTM plugin); `test_vector_realloc` data corruption from `std::vector` reallocation races (not solved by TM instrumentation)
