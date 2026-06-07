# EBR Double-Free Analysis

## Status: FIXED (June 2026)

Two interacting bugs caused double-free heap corruption in TinySTM EBR:

### Bug 1: `commit_version = 0` for read-only TX

**Root cause:** `tinystm_wbctl.hpp:commit()` initialized `commit_version = 0` instead of the current global clock. Read-only transactions never increment the clock, so `commit_version` stayed 0. EBR uses this version to decide when it is safe to free retired pointers; version 0 is always considered "safe", so retired entries from read-only TXs were freed immediately while other threads might still access them.

**Fix:** `tinystm_wbctl.hpp:213` — `volatile word_t commit_version = get_clock();`

### Bug 2: Shared `std::vector` buffer in deferred free lists

**Root cause:** When two threads both call `_M_realloc_insert_tm_clone` on the same shared vector, both see the same old buffer pointer (via snapshot isolation), both call `_M_deallocate_tm_clone` → `tm_free` on that pointer, and the pointer ends up in both threads' deferred-free lists. Each thread treats it as "owned" and later frees it → double-free.

The STL vector's data buffer is on the regular heap (allocated via `::operator new`, not `tm_malloc` because STL internal `new` is intentionally not intercepted — `handleMallocFree` at `tm_instrument_helpers.hpp:329`). The buffer is freed via `tm_free` which defers. Since both threads independently defer the same pointer, the double-free manifests at `tm_flush_retired_frees` time.

**Fix:** A global mutex-protected set (`g_retired_global_set`) in `tm_move_deferred_to_retired` ensures each pointer is retired by exactly one thread. The loser thread discards its `FreeNode` bookkeeping entry.

### Verification

| Test | Status |
|------|--------|
| `test_vec_push` (2t×100 iter) | PASS |
| `test_simple_vector` | PASS |
| `test_vector_realloc` | PASS |
| `test_alloc_stress` (single vec worker) | PASS |

### Known remaining crash: `test_alloc_stress` with 2+ vec workers

With 2+ concurrent vector workers, `test_alloc_stress` still crashes with SIGSEGV after ~3 seconds. The crash is in `tm_write_i8` (called from `construct_at_tm_clone` inside `_M_realloc_insert_tm_clone`) trying to direct-write to an unmapped address.

The root cause is a race between two threads both executing `_M_realloc_insert_tm_clone` on the same shared vector. Since:
- `_M_allocate_tm_clone` → `::operator new` (regular heap, per `new_allocator::allocate_tm_clone`)
- `_M_deallocate_tm_clone` → `tm_free` (deferred via EBR)
- `construct_at_tm_clone` → `tm_write_i8` → direct write (non-TM-region address)

The new buffer returned by `::operator new` should be valid, but under concurrent load a thread may attempt to write to an address that was freed by the other thread's retired-list flush (via `::operator delete`).

A secondary issue is that `_M_allocate_tm_clone` uses `::operator new` while `_M_deallocate_tm_clone` uses `tm_free`. This asymmetry means the new buffer is on the regular heap (not TM region), so `tm_write_i8` does a direct write. If two threads both reallocate the same vector, one thread's new buffer may overlap with the other thread's old buffer that was just freed via EBR retirement.

**Status:** Not yet fixed. The test uses `ITEMS_PER_VEC_TX = 200` per push_back call, which causes frequent reallocation. Reducing run duration below the crash threshold (-d 2) avoids the issue. Single-threaded passes reliably.

### Relevant files

- `backends/tm_impl/tiny_stm/tinystm_wbctl.hpp` — `commit_version` fix
- `backends/tm_impl/tiny_stm/TinySTM_runtime.cpp` — EBR pipeline, `tm_free`
- `backends/tm_impl/common/tm_alloc_overrides.hpp` — Global retired set, defer/retire/flush
- `backends/tm_impl/tiny_stm/tinystm_common.hpp` — `commit_version` field on Transaction
