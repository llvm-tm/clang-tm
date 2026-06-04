# Session Summary

## Latest Session (this session — 2026-06-04)

### Fix: TinySTM runtime nesting counter + expli API commit (stack write-back)

#### Root cause: Double-redirection of nesting counter
`TinySTM_runtime.cpp` used `ts->nested_call_counter` (from `TMThreadState`, allocated in TM address space via `tm_get_thread_state()`) while the expli API (`tx_executor.hpp`, `tm_api.hpp`) used the bare `__thread int32_t tm_nested_call_counter`. The runtime never saw the counter set by the expli API — `tinystm::begin()` was never called, and transactions silently did nothing.

**Fix**: Sync both counters in `tm_begin()`: read whichever was set (expli → `tm_nested_call_counter`, plugin → `ts->nested_call_counter` via GEP), propagate to both. Do NOT modify `tm_nested_call_counter` in `tm_end()` — the expli API manages it.

#### Root cause: Stack-address write-back blocked
In commit's write-back phase, `is_stack_addr(addr) continue;` skipped all stack addresses. This was correct for the **plugin** (dead `_tm_clone` frames) but wrong for the **expli API** (valid stack-allocated `expli::TM<T>` fields that need write-back).

**Fix**: Added `thread_local bool g_tm_expli_mode` flag. Set to `true` in `tm_begin()` when the expli API path is detected (`tm_nested_call_counter > 0 && ts->nested_call_counter == 0`), `false` for the plugin path (`ts->nested_call_counter > 0 && tm_nested_call_counter == 0`). The write-back loop skips stack addresses only when `!g_tm_expli_mode`.

#### Test results
- **Expli C++**: 114/114 TX tests PASS, 207/207 DS tests PASS (both `-O0 -g`)
- **LLVM plugin**: `test_treap_tx` PASS (H, I, J printed, "Result: PASS", read-set=69, write-set=66)
- **Other TinySTM plugin tests**: `test_ll_alloc` PASS, `test_simple_vector` PASS, `test_construct_tx_pattern` PASS, `test_local_containers` PASS
- **Pre-existing issues unchanged**: `test_vec_push` and `test_alloc_stress` hang at exit; `test_vector_realloc` has data corruption from `std::vector` reallocation races

#### Key changes
- `backends/runtimes/TinySTM_runtime.cpp`: Added `tm_nested_call_counter` definition, `g_tm_expli_mode` flag, path-detection logic in `tm_begin()`, fixed counter sync
- `backends/TinySTM/tinystm_wbctl.hpp`: Added `extern thread_local bool g_tm_expli_mode;` declaration, guarded `is_stack_addr` in write-back phase
- `backends/TinySTM/tinystm_wbctl.hpp`: Removed is_stack_addr/null-addr guards from lock acquisition and lock release (already safe via `is_locked_by` check — stack addresses were never locked, so `is_locked_by` returns false and skips them)

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

## Next Steps
1. Remove vestigial `extern __thread int32_t tm_nested_call_counter` from `tinystm_wbctl.hpp` line 24 (now unused — the variable is defined in `TinySTM_runtime.cpp` and declared by `tm_api.hpp` for the expli API, `tm_thread_state.hpp` for the plugin)
2. (Future) Fix LLVM plugin TLV interaction: BC's `external thread_local` relocations misindexed when merged with TinySTM's larger TLV descriptor table
3. (Future) Implement the plugin alias analysis to skip stack-address instrumentation entirely (remove the root cause at the source)

## Issues Found
1. **Double-redirection of nesting counter in `TinySTM_runtime.cpp`**: Originally used `__thread int32_t tm_nested_call_counter` directly, but a prior refactor changed to `ts->nested_call_counter` (from `TMThreadState`), breaking the expli API path. Now fixed with bidirectional sync in `tm_begin()`.
2. **Stack write-back blocked for expli API**: `is_stack_addr(addr) continue;` in commit's write-back phase skipped stack addresses unconditionally, preventing expli API `expli::TM<T>` fields from being persisted. Fixed with `g_tm_expli_mode` flag.
3. **Pre-existing issues**: `test_vec_push` / `test_alloc_stress` hang at exit (TinySTM plugin); `test_vector_realloc` data corruption from `std::vector` reallocation races (not solved by TM instrumentation)
