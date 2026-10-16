# API Reference

The public surfaces of the three instrumentation paths, the Rust runtime, and
how a backend is selected. For a task-oriented walkthrough see
[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md); for per-backend algorithm details see
[IMPLEMENTATIONS.md](IMPLEMENTATIONS.md).

There are four ways to run code inside a transaction:

| Path | You write | Instrumentation | Headers / crate |
|------|-----------|-----------------|-----------------|
| C++ explicit, typed | `expli::TM<T>` | explicit calls, no compiler | `explicit_api/cpp/expli_tm_api/tm_api.hpp` |
| C++ raw hooks | `tm_read_i4` / `tm_write_i4` … | explicit calls, no compiler | `backends/tm_impl/common/tm_hooks.hpp` |
| C++ LLVM plugin | `TM` / `TX` / `THREAD` annotations | LLVM 22 pass rewrites loads/stores | `plugin/` |
| Rust | `tm::transaction` + `tm::TmCell<T>` | explicit calls, no compiler | crate `tm` (`explicit_api/rust/workspace`) |

All four ultimately dispatch through the same 23-function `TMRealHooks` table,
so a program can be built against any backend with `BACKEND=<NAME>`.

---

## 1. C++ explicit, typed API (`expli::TM<T>`)

`#include "expli_tm_api/tm_api.hpp"`, then use `expli::TM<T>`.
`tm_init` / `tm_init_thread` / `tm_exit_thread` / `tm_exit` are process/thread
lifecycle calls; the backing store is allocated from the TM region on first use.

```cpp
expli::TM<int> counter;              // a TM-managed int

expli::TM<int>::init();
expli::TM<int>::thread_init();
expli::TM<int>::transaction([] {
    int v = counter.read();          // tm_read_i4
    counter.write(v + 1);            // tm_write_i4
});
expli::TM<int>::thread_exit();
expli::TM<int>::exit();
```

### Members (primary template `TM<T>`)

| Member | Meaning |
|--------|---------|
| `begin()` / `end()` | Enter / leave the TX. **Do not** call directly around a body that can abort — a conflicting abort `siglongjmp`s to the last `sigsetjmp`, which these do not establish. Use `transaction()`. |
| `transaction(F)` | `sigsetjmp` in its own frame + retry loop; the safe way to run a body that may abort. |
| `init()` / `exit()` / `thread_init()` / `thread_exit()` | Forward to the runtime lifecycle hooks. |
| `malloc` / `calloc` / `realloc` / `free` | Static wrappers; inside a TX `tm_malloc` routes through the region allocator. |
| `read()` / `write(v)` | TM read/write of this object. |
| `read_at(addr)` / `write_at(addr, v)` | Static read/write of an arbitrary `T*`. |
| `peek()` / `poke(v)` | **Untracked** raw access (debugging / non-TX paths only). |

### `TM<T*>` (pointer / array specialization)

Backed by a `tm_write_ptr`-tracked slot; the buffer itself is plain heap memory
allocated with `::operator new` (see the comment in the header for why — a
`tm_malloc` buffer would be reaped by `tm_clear_spec_allocs()` on the next
`tm_begin()`, a use-after-free).

| Member | Meaning |
|--------|---------|
| `alloc(n)` | Allocate `n` elements with `::operator new`, placement-construct each. |
| `free_ptr()` | `::operator delete` the current buffer. |
| `read()` / `write(p)` | Tracked load/store of the pointer slot. |
| `operator[](i)` | `read()[i]` — element access (works when `T = TM<U>`). |

### Containers and helpers

- `tm_map.hpp` — `flat_map<K,V>`, `flat_multimap<K,V>`, `flat_set<K>`.
- `tm_treap_multimap.hpp`, `explicit_hashmap.hpp`, `explicit_rbtree.hpp`,
  `explicit_sorted_list.hpp` — TM-aware associative/sequence structures.
- `containers/tm_bit_vector.hpp`, `containers/tm_heap.hpp`,
  `containers/tm_small_set.hpp` — auxiliary containers.
- `memory_access.hpp` — `MemoryAccess<bool UseTM>` so one data-structure
  implementation compiles both in-TX (`tm_read_*`) and out-of-TX (plain load).
- `tm_concurrent_map.hpp`, `tx_executor.hpp`, `thread_barrier.hpp` — concurrency
  helpers.

---

## 2. C++ raw hook ABI

`#include "tm_hooks.hpp"`. These are **function pointers**, so a backend must
never define a function of the same name (`void tm_begin(){}` collides with
`void (*tm_begin)()`); backend implementations use `static void real_tm_begin()`
and publish them via the hooks table.

### Hooks

```c
void tm_init(void);          void tm_exit(void);
void tm_init_thread(void);   void tm_exit_thread(void);
void (*tm_begin)(void);      void (*tm_end)(void);
void (*tm_set_jmpbuf)(void *);

void *(*tm_malloc)(size_t);
void *(*tm_calloc)(size_t, size_t);
void *(*tm_realloc)(void *, size_t);
void (*tm_free)(void *);

uint8_t  (*tm_read_i1)(uint8_t*);   uint16_t (*tm_read_i2)(uint16_t*);
uint32_t (*tm_read_i4)(uint32_t*);  uint64_t (*tm_read_i8)(uint64_t*);
float    (*tm_read_f4)(float*);     double   (*tm_read_f8)(double*);
void    *(*tm_read_ptr)(void**);

void (*tm_write_i1)(uint8_t*, uint8_t);    void (*tm_write_i2)(uint16_t*, uint16_t);
void (*tm_write_i4)(uint32_t*, uint32_t);  void (*tm_write_i8)(uint64_t*, int64_t);
void (*tm_write_f4)(float*, float);        void (*tm_write_f8)(double*, double);
void (*tm_write_ptr)(void**, void*);
```

### Registering a backend

```cpp
#include "tm_hooks.hpp"

static void  real_tm_begin() { /* ... */ }
static uint8_t real_tm_read_i1(uint8_t *a) { /* ... */ }

static const TMRealHooks g_my_backend_hooks = {
    .begin = real_tm_begin, /* ... all 23 fields ... */
    .read_i1 = real_tm_read_i1, /* ... */
};

void tm_init() { tm_register_real_hooks(&g_my_backend_hooks); }
```

The two canonical field lists that must stay in sync live next to the struct:
`TM_HOOK_FIELDS(X)` (all 23) and `TM_HOOK_ACCESS_FIELDS(X)` (the 20 data-access
hooks). The runtime's own tables are generated from these macros in
`backends/tm_impl/common/tm_hooks.cpp`.

### Runtime control

- `tm_register_real_hooks(const TMRealHooks*)` — install a backend's hooks.
- `tm_swap_runtime(name)` — phase-based swap of the active backend at runtime.
- `tm_set_num_threads(n)` — declare the thread count before `tm_init`.
- `tm_region_malloc(...)` / `tm_track_spec_alloc(...)` — region allocation and
  speculative-allocation tracking (speculative entries are cleared on abort).
- Shared TLS in `tm_hooks.cpp`: `tm_jmpbuf`, `tm_nested_call_counter`,
  `tm_longjmp_ret`.

In `LLVM_TM_PLUGIN` builds `tm_init` / `tm_exit` / `tm_init_thread` /
`tm_exit_thread` are **data** (function pointers), not text functions — keep the
`#ifdef LLVM_TM_PLUGIN` wrappers.

---

## 3. C++ LLVM plugin

Compile with the plugin attached and annotate declarations (see
`tests/plugin/tm_test_common.hpp`):

```cpp
#define TM     __attribute__((annotate("tm")))               // transactional fn
#define TX     __attribute__((annotate("shared"), noinline)) // shared access
#define THREAD __attribute__((annotate("thread"), noinline)) // thread entry
#define MAIN   __attribute__((annotate("main"),   noinline)) // main entry

TX int shared_counter;
TX int inc(int n) { return n + 1; }
THREAD void worker();
MAIN  int main();
```

The pass (LLVM 22) instruments every plain load/store in `TX`/`TM` code into
`tm_read_*` / `tm_write_*` and wraps `THREAD` bodies in `tm_begin`/`tm_end`.
Build/test with `make -C plugin run`.

---

## 4. Rust API (crate `tm`)

`tm` re-exports every backend as a module and provides a backend-agnostic
vocabulary. It requires the `std` runtime; the simulator build gates TLS behind
`#[cfg(feature = "simulation")]`.

```rust
use tm::{transaction, TmCell};

let counter = TmCell::new(0i64);
transaction(|tx| {
    let v = tx.read(&counter);
    tx.write(&counter, v + 1);
});
```

| Item | Meaning |
|------|---------|
| `TmPrimitive` | Marker trait (`Copy + Send + Sync + 'static`) for values that can be read/written through the raw byte hooks. |
| `TmCell<T: TmPrimitive>` | `new`, `ptr`, `read_raw(dst)`, `write_raw(src)` — a TM-managed cell. |
| `TmPtr<T>` | `new(ptr)`, `null()`, `get()` — a TM-managed pointer slot. |
| `Transaction` | `read`, `write`, `read_raw`, `write_raw` — passed into the body. |
| `transaction(f)` | Runs `f(&Transaction)` with begin/abort-retry/commit. |
| `TmRaw` | Escape hatch re-exported from `runtime-core` for raw hook access. |

Backends are exported as `runtime_*` modules (`runtime_norec`, `runtime_tl2`,
`runtime_swisstm`, …); the `tm` crate re-exports their public items.

---

## 5. Selecting a backend

```sh
make -C benchmarks/cpp bin/test_tx BACKEND=NOREC
```

`BACKEND` names the hooks that get linked: `TINYSTM WBETL WT NOREC NORECBF
SWISSTM TL2 TSC_TM MVLOG SGL LEFTRIGHT ROMULUS XTM SPHT TSXSGL` plus the GPU
family (`GPU_STM_CPU`, `CSMV`, …). See [IMPLEMENTATIONS.md](IMPLEMENTATIONS.md)
for what each implements and which are x86-only.
