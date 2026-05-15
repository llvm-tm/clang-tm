# Memory Allocation Capture Assessment

## Current Status: NOT Captured

The TM plugin does NOT intercept any heap allocations.  The test
`/tmp/alloc_test.cpp` confirms this: after instrumentation, every
`malloc`, `free`, `_Znwm` (operator new), `_ZdlPvm` (operator delete)
call remains unchanged.  The only new calls inserted are `tm_read_*` /
`tm_write_*` for TM-annotated globals.

## Why This Matters

For PersistentSGL and DistributedSGL to work with heap-based containers
(std::map, std::vector, etc.), allocations must go to the persistent
mmap region.  Currently:

* `std::map` tree nodes are allocated via `operator new` → process heap
* These nodes contain raw C++ pointers to other nodes
* After restart, the heap is gone and the pointers dangle

The `persistent_kv_stdmap.cpp` benchmark works around this by
serialising to arrays — the std::map is rebuilt from scratch on each
init.

## What Would Be Required for Full Capture

### 1. Runtime hooks (new file or in tm_runtime_hooks.hpp)

| Hook | Signature | Purpose |
|------|-----------|---------|
| `tm_malloc(size_t)` | `void*` | Allocate from persistent mmap |
| `tm_free(void*)` | `void` | Deallocate |
| `tm_calloc(size_t, size_t)` | `void*` | Allocate + zero |
| `tm_realloc(void*, size_t)` | `void*` | Resize |
| `tm_new(size_t)` | `void*` | Like malloc, for operator new |
| `tm_delete(void*)` | `void` | Like free, for operator delete |

### 2. Plugin pass change (TMInstrumentPass.cpp)

In the instruction loop, add:
```cpp
if (isHeapAllocationCall(Call)) {
    // Replace: CallInst malloc(n) → tm_malloc(n)
    //          CallInst new T    → tm_new(sizeof(T))
} else if (isDeallocationCall(Call)) {
    // Replace: CallInst free(p) → tm_free(p)
    //          CallInst delete p → tm_delete(p)
}
```

The detection infrastructure already exists in `tm_local_vars.hpp`
(`isHeapAllocationCall`, `isDeallocationCall`) but is only used to
*exclude* allocations from TM instrumentation (treating non-escaping
in-function allocations as local).

### 3. Backend: PersistentAllocator

Each runtime backend would implement these hooks.  For PersistentSGL:
```cpp
void* tm_malloc(size_t sz) {
    // bump-allocate from the persistent mmap
    size_t off = atomic_fetch_add(&heap_used, sz);
    return mmap_base + off;
}
void tm_free(void*) { /* bump allocator: no-op */ }
```

### 4. Caller/callee distinction

Allocator interception should happen in **all functions** (not just
cloned ones), because:
* TX functions call allocators directly (e.g. `new` inside a transaction)
* Cloned non-TX functions also call allocators (e.g. `std::map` internals)

Non-TX, non-cloned functions should also get their allocators replaced
for consistency (a non-TX helper that allocates memory used by a TX
function).

### 5. `std::map` specific note

Even with `tm_malloc`/`tm_free` intercepting allocations, `std::map`'s
internal tree nodes store RAW POINTERS to other nodes.  After restart,
these pointers are invalid (heap addresses from the previous run).

Two solutions:
1. **Fixed-address mmap**: map the persistent heap at a deterministic
   VA with MAP_FIXED.  Fragile but avoids pointer relocation.
2. **RelPtr container**: use a container (like `std::map` with a custom
   allocator) that stores relative offsets instead of raw pointers.
   The `RelPtr<T>` class in `backends/rel_ptr.hpp` provides the offset
   primitive; a `PersistentMap` wrapper would be needed to use it.
3. **Serialisation**: rebuild the container from arrays on each restart
   (the approach used by `persistent_kv_stdmap.cpp`).

## Test Program

`/tmp/alloc_test.cpp` (preserved on disk) exercises:
* Direct `malloc`/`free`
* C++ `new`/`delete`
* `std::map` operations (internal `new`/`delete`)

The instrumented IR (`/tmp/alloc_test.instr.ll`) proves zero allocator
interception occurs.

## Effort Estimate

| Component | Estimated complexity |
|-----------|---------------------|
| Declare hooks in tm_runtime_hooks.hpp | ~20 lines |
| Plugin pass: replace alloc CallInst | ~50 lines |
| Implementation in one backend (PersistentSGL) | ~40 lines |
| Implementation in other backends | ~20 lines each |
| RelPtr-based persistent container (for std::map) | ~100 lines |
| Testing and debugging | Significant (allocation correctness is subtle) |

**Total: roughly 200–300 lines of new code** for a basic implementation
that works with both C `malloc`/`free` and C++ `operator new`/`delete`.
`std::map` requires either a RelPtr container or fixed-address mmap
for correctness.
