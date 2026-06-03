# Plan: TM Address Space Allocator + Stack-Pointer Argument Validation

## 1. Problem Statement

### 1.1 Stale Stack Pointers in Queue Mode
When a TX function is called via `tm_enqueue`, its packed arguments are copied to a heap struct and dispatched to a worker thread. If any argument is a pointer to the **caller's stack** (local variable, alloca, or any GEP thereof), that pointer will be dangling by the time the worker thread executes the dispatch wrapper.

**Example of dangerous code that currently compiles silently:**
```cpp
__attribute__((annotate("transaction"), noinline))
void tx_insert(Node *n) { /* ... */ }

void caller() {
    Node tmp;                  // stack local
    tx_insert(&tmp);           // dangling pointer on worker thread!
}
```

### 1.2 Distinguishing TM Memory from Non-TM Memory
Currently the runtime uses `isStackAddress(addr)` to detect and bypass TM instrumentation for stack-address loads/stores. This check is:
- **Fragile**: platform-specific (pthread_getattr_np on Linux, pthread_get_stackaddr_np on macOS)  
- **Incorrect**: It only catches stack addresses but not other non-TM heap allocations (e.g., `std::vector` internal buffers that should NOT go through TM).
- **Read-side gap**: The check is only applied to WRITES in some backends; READS from stack addresses are still TM-instrumented in others, corrupting read-set validation.

A dedicated TM address space would replace the negative check ("is this address on the stack?") with a positive check ("is this address in TM memory?"). This is both simpler and more correct.

---

## 2. Compiler Errors for Stack-Pointer Arguments

### 2.1 Where to Add the Check
In the queue pipeline pass (`TMQueueGlobalInitPass`), at the point where we replace a TX-function call site with `tm_enqueue` (step 5, `replaceCallWithEnqueue`), trace each pointer argument back:

1. If the argument is an `AllocaInst` → error
2. If the argument is a GEP whose base is an `AllocaInst` → error  
3. If the argument is a `LoadInst` that loads from an alloca-stored pointer → error
4. If the argument is a `BitCast` → strip and check the operand recursively

### 2.2 Error Message
```
error: transaction function 'tx_insert' called with pointer to stack-allocated
memory as argument #0. In queue mode, the transaction runs on a worker thread
and the stack address 'tmp' will be invalid. Allocate with tm_malloc instead.
```

### 2.3 Implementation Details
- Create a new function `argTracesToStack(Value *Arg)` in `tm_instrument_helpers.hpp` (or directly in the queue pass code).
- It walks through `GEPOperator`, `BitCastOperator`, `AddrSpaceCastOperator`, and `LoadInst` (with depth limit of 5 to prevent infinite recursion on cycles).
- If it reaches an `AllocaInst`, return true.
- The check runs after we've identified a call site to replace (i.e., in the loop at step 5 of `TMQueueGlobalInitPass`).
- Only emit the error for the queue pipeline — the inline and non-inline pipelines run the TX on the calling thread, so stack addresses are valid.

### 2.4 Test Cases
| Test | Expected Result |
|------|----------------|
| `tx_func(stack_local)` | Compile-time error |
| `tx_func(&stack_local.field)` | Compile-time error |
| `tx_func(ptr_loaded_from_stack)` | Compile-time error |
| `tx_func(heap_ptr)` | OK (enqueue) |
| `tx_func(tm_malloc_result)` | OK (enqueue) |
| Same checks in inline pipeline | OK (no error — stack is valid) |

---

## 3. TM Address Space Allocator

### 3.1 Architecture

```
┌──────────────────────────────────────────────┐
│           Process Virtual Address Space       │
│                                                │
│  ┌──────────────────────────────────────┐     │
│  │         TM Region (e.g., 64 GB)       │     │
│  │  g_tm_region_start            g_end   │     │
│  │  [ TX data ][ TX data ]...[ free ]    │     │
│  └──────────────────────────────────────┘     │
│                                                │
│  ┌────────────────────┐                       │
│  │   Regular heap     │  (malloc, new, STL)   │
│  └────────────────────┘                       │
│                                                │
│  ┌────────────────────┐                       │
│  │      Stack         │                       │
│  └────────────────────┘                       │
└──────────────────────────────────────────────┘
```

### 3.2 Region Allocation (One-Time at `tm_queue_init`)

On first call to `tm_queue_init` (or `tm_init`):

```
g_tm_region_start = mmap(
    /* hint_addr */ nullptr,
    /* length    */ TM_REGION_SIZE,
    /* prot      */ PROT_READ | PROT_WRITE,
    /* flags     */ MAP_PRIVATE | MAP_ANONYMOUS,
    /* fd        */ -1,
    /* offset    */ 0
);
g_tm_region_end = g_tm_region_start + TM_REGION_SIZE;
```

- No `MAP_FIXED` — let the kernel choose the address (safe, no conflict with ASLR).
- `TM_REGION_SIZE` defaults to 64 GB (`0x1000000000`).
- The OS only commits pages on first touch — reserving 64 GB costs only virtual address space.
- On 32-bit platforms, fall back to a smaller size (e.g., 512 MB).

### 3.3 tm_malloc / tm_calloc / tm_realloc / tm_free

Replace the current `::operator new`-based allocator with a TM-region bump allocator:

```c
// Bump (linear) allocator for TM region
static struct {
    char *start;     // g_tm_region_start
    char *end;       // g_tm_region_end
    char *bump;      // next free byte (atomic for multi-threaded allocation)
} tm_heap;

void *tm_malloc(size_t sz) {
    // Align to 16 bytes
    sz = (sz + 15) & ~15;
    char *p = __atomic_fetch_add(&tm_heap.bump, sz, __ATOMIC_RELAXED);
    if (p + sz > tm_heap.end) {
        fprintf(stderr, "FATAL: TM region exhausted\n");
        abort();
    }
    return p;
}

void tm_free(void *ptr) {
    // Bump allocator: free is a no-op (memory is reused on next program run
    // or we add a free-list for long-running workloads).
}
```

**Why bump allocator?** TM workloads tend to allocate heavily during a TX and free on commit/abort. A bump allocator is O(1) for both alloc and free (when free is a no-op). For workloads with long-lived allocations, add a slab-based free-list in a later iteration.

**Deferred frees**: On TX abort, spec_alloc entries pointing into the TM region are simply discarded (no need to return to a free list since the bump is not rewound — the spec_alloc bookkeeping already prevents use-after-free within the TX). For long-running programs that exhaust the region, add epoch-based reclamation.

### 3.4 isTMAddress() — Replacement for isStackAddress()

```c
static inline bool isTMAddress(const void *addr) {
    return addr >= g_tm_region_start && addr < g_tm_region_end;
}
```

Replace all `isStackAddress(addr)` calls in the backends with `!isTMAddress(addr)`:

```
// Old (WBCTL write_word_ctl):
if (isStackAddress(addr)) { raw_store; return; }

// New:
if (!isTMAddress(addr)) { raw_store; return; }
```

**Advantage**: Non-TM reads are also caught correctly. In backends like SwissTM (eager-read), `read_impl` can now skip TM for addresses outside the region, preventing the null-address crashes from moved-from STL objects.

### 3.5 Thread-Safe Allocation During TX

TX-internal allocations (`tm_malloc` called from within a TX) go to the bump allocator. The bump pointer is updated atomically. To support rolling back spec_alloc on abort without rewinding the bump, we keep a TX-local list of allocated ranges (or just the start-of-TX bump position, and reset on abort — this is simpler and handles the common case: TX-internal allocations are freed on abort anyway).

However, resetting the bump on abort would lose any concurrent allocations from other threads in the same region. So we keep per-thread sub-allocators or use a free-list approach for production. For the initial implementation, a single atomic bump with `tm_clear_spec_allocs` as a no-op (just forget the entries) is sufficient.

### 3.6 Address Sanitizer for TM Region

In debug mode (`-UNDEBUG` or explicit `-DTM_ADDRESS_SANITIZE`):

- Maintain a parallel bitmap (`tm_alloc_bitmap[N]` where N = TM_REGION_SIZE / PAGE_SIZE) that marks which pages are allocated.
- On each `tm_read`/`tm_write`, check that the address either falls within a TM-allocated block or is outside the TM region entirely.
- On `tm_free`, do NOT clear the bitmap (deferred reuse) — instead, keep a separate poisoned range list for use-after-free detection.
- On commit write-back, verify that every address in the write-set is within an allocated TM block.

This catches:
- Use-after-free within the TM region
- Writing to unallocated TM region pages
- Reading from TM-adjacent memory that was never allocated

### 3.7 Platform Header (`tm_platform.hpp`)

The existing `backends/tm_platform.hpp` gets new entries:

```cpp
// ---- TM Region (address space allocator) ----

#ifndef TM_REGION_SIZE
#define TM_REGION_SIZE (64ULL * 1024 * 1024 * 1024) // 64 GB
#endif

// Initialize the TM address space region (called once at startup).
// Returns 0 on success, -1 on failure.
int tm_platform_region_init(void * *out_start, void * *out_end);

// Check if an address falls within the TM region.
static inline bool tm_region_contains(const void *addr,
                                       const void *region_start,
                                       const void *region_end) {
    return addr >= region_start && addr < region_end;
}
```

Platform-specific implementation in the `.cpp` companion:

| Platform | Implementation |
|----------|---------------|
| Linux / macOS / BSD | `mmap(nullptr, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)` |
| Windows | `VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE)` |
| WASM / bare-metal | Fallback to regular `malloc` + mark no region (returns -1, runtime falls back to `isStackAddress` logic) |

**Adding a new platform** follows the documented convention in `tm_platform.hpp` (add `#elif defined(NEW_PLATFORM)` in the region init block).

---

## 4. Runtime Changes

### 4.1 Files to Create
- `backends/tm_region_allocator.hpp` — header with `tm_region_init()`, `isTMAddress()`, `tm_region_malloc()`/free/realloc declarations (inline or macro).
- `backends/runtimes/tm_region_allocator.cpp` — implementation (mmap + bump allocator).

### 4.2 Files to Modify
| File | Change |
|------|--------|
| `backends/tm_platform.hpp` | Add `TM_REGION_SIZE` constant, `tm_platform_region_init()` declaration |
| `backends/tm_common.hpp` | Add `#include "tm_region_allocator.hpp"`, replace `isStackAddress` calls with `!isTMAddress` |
| All 8 backend `.hpp` files | Replace `isStackAddress(addr)` with `!g_tm_region_start \|\| !isTMAddress(addr)` (fallback: if region not initialized, skip check) |
| `backends/runtimes/TinySTM_runtime.cpp` (and all runtimes) | Call `tm_region_init()` in `init()`, replace `tm_malloc`/`tm_calloc`/`tm_realloc`/`tm_free` with region variants |
| `backends/runtimes/queue_runtime.cpp` | Call `tm_region_init()` in `tm_queue_init` |
| `llvm_tm_plugin/src/tm_instrument_helpers.hpp` | Add `argTracesToStack()` helper for the queue pass |

### 4.3 Inline vs Queue Mode
- In **inline mode** (existing behavior): malloc/free still goes through the region allocator if initialized; `isTMAddress` check works identically.
- In **queue mode**: worker threads initialize their TLS and use the same region. All TM data lives in the same shared region accessed by all workers.

---

## 5. Plugin Changes

### 5.1 Stack-Pointer Argument Validation
In `TMQueueGlobalInitPass::run`, step 5 (call site replacement), add before the `replaceCallWithEnqueue` call:

```cpp
// Validate args: no stack pointers in queue mode
for (unsigned i = 0; i < Call->arg_size(); i++) {
    Value *Arg = Call->getArgOperand(i);
    if (Arg->getType()->isPointerTy() && argTracesToStack(Arg)) {
        F.getContext().diagnose(DiagnosticInfo(
            DS_Error, "transaction function '" + Orig->getName() +
            "' called with pointer to stack-allocated memory as argument #" +
            Twine(i) + ". In queue mode, the transaction runs on a worker "
            "thread and the stack address will be invalid. "
            "Allocate with tm_malloc instead."
        ));
    }
}
```

The `argTracesToStack` function traces through:
- `AllocaInst` → return true
- `GEPOperator` → recurse on base pointer
- `BitCastOperator` / `AddrSpaceCastOperator` → recurse on operand
- `LoadInst` (with depth ≤ 3) → trace the pointer operand of the load
- `CallBase` → trace the return value if it's from `alloca` (unlikely but possible via inlining)
- Anything else → return false

### 5.2 Optional: Address-Space Annotation
Users can annotate global variables with `__attribute__((annotate("tm_shared")))` to indicate they should be placed in the TM region. The plugin could:
1. Recognize the annotation at module level
2. Move the global to a specific section (`.tm_shared`) via `@llvm.global_ctors` callback
3. The linker script / runtime loads this section into the TM region

This is a future enhancement — the initial implementation skips it and relies on `tm_malloc` for all dynamic TM data.

---

## 6. Unit Tests

### 6.1 Stack-Pointer Validation Tests
| Test File | Description |
|-----------|-------------|
| `test_queue_stack_arg.cpp` | TX function called with stack-local → compile-time error expected |
| `test_queue_heap_arg.cpp` | TX function called with `tm_malloc`'d ptr → OK |
| `test_queue_gep_arg.cpp` | TX called with `&local.field` → error |
| `test_queue_load_arg.cpp` | TX called with pointer loaded from stack → error |
| `test_queue_noarg.cpp` | TX with no args → OK |
| `test_inline_stack_ok.cpp` | Same code using inline pipeline → OK (no error) |

### 6.2 TM Address Space Tests
| Test File | Description |
|-----------|-------------|
| `test_region_init.cpp` | `tm_region_init` returns valid non-null region, size ≥ TM_REGION_SIZE |
| `test_region_alloc.cpp` | `tm_malloc(100)` returns address within region, write + read preserves value |
| `test_region_multi.cpp` | Multiple threads allocate from region, no overlap |
| `test_region_not_tm.cpp` | Regular `malloc`'d address → `isTMAddress()` returns false |
| `test_region_stack.cpp` | Stack address → `isTMAddress()` returns false |
| `test_region_exhaust.cpp` | Allocate until region full → fatal error |

### 6.3 Integration Tests
| Test File | Description |
|-----------|-------------|
| `test_queue_bank.cpp` | Bank benchmark with queue pipeline (transfer money between accounts in TM region) |
| `test_queue_async.cpp` | Async TX + manual `tm_wait_prev_tx` |

---

## 7. Implementation Order

| Phase | What | Depends On |
|-------|------|------------|
| **1** | `tm_region_allocator.hpp` + `.cpp` (mmap + bump allocator) | `tm_platform.hpp` |
| **2** | `isTMAddress()` + replace `isStackAddress` in all backends | Phase 1 |
| **3** | Wire `tm_region_init()` into `tm_init()` and `tm_queue_init()` | Phase 1 |
| **4** | `tm_malloc`/`tm_calloc`/`tm_realloc`/`tm_free` → use region | Phase 2 |
| **5** | `argTracesToStack()` in plugin | — |
| **6** | Stack-pointer validation in queue pass | Phase 5 |
| **7** | Debug address sanitizer (bitmap + bounds checks) | Phase 2 |
| **8** | Bump allocator → free-list for long-running workloads | Phase 4 |

---

## 8. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| 64 GB virtual reservation fails on constrained systems | TM doesn't initialize | Fall back to < 1 GB or disable TM; print clear error |
| Bump allocator exhausts region in long-running workloads | Fatal abort | Add free-list before production use; monitor usage with `tm_region_usage()` |
| Performance regression from `isTMAddress()` check on every TM op | 1-2 loads + 2 compares (tiny) | Same cost as current `isStackAddress`; keep inline |
| STL containers' internal allocations go outside TM region | False negatives in `isTMAddress` | This is CORRECT — STL internals should NOT be TM-tracked. The region check catches this naturally. |
