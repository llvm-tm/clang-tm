# TM API C++ — LLVM Transactional Memory Plugin

An LLVM compiler plugin that automatically instruments C++ code for Software
Transactional Memory (STM) using source-level annotations. Mark globals with
`__attribute__((annotate("tm")))` and function with
`__attribute__((annotate("transaction")))` — the plugin handles the rest.

## Quick Start

## One-line install

```bash
curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/llvm_tm_plugin/bootstrap-install.sh | bash
```

Custom prefix:

```bash
curl -fsSL ... | bash -s -- --prefix ~/.local
```

## One-line uninstall

```bash
curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/llvm_tm_plugin/bootstrap-uninstall.sh | bash
```

## Standalone benchmark workspace

```bash
/usr/local/lib/clang-tm/install-benchmarks.sh                # creates ~/tm-benchmarks/
/usr/local/lib/clang-tm/install-benchmarks.sh --benchdir ~/my-benchmarks

# Run the money-conservation test:
make -C ~/tm-benchmarks test
```

## From a local clone

```bash
cd llvm_tm_plugin
make variants                           # build all 7 plugin variants
./install.sh                            # installs to /usr/local/
PREFIX=~/.local ./install.sh            # user-local prefix

# Set up standalone benchmarks
./install-benchmarks.sh

# Run correctness test:
make -C ~/tm-benchmarks test

# Uninstall:
./uninstall.sh
```

## Prerequisites

- **LLVM 22+** with the `opt` tool and development libraries
- **Clang 16+** (matching LLVM version)
- **Python 3.8+** (for `tm-resolve-opaque.py`)
- `make`, `gtimeout` (macOS: `brew install coreutils`) / `timeout` (Ubuntu: pre-installed in `coreutils`)

## Building

### 1. Build the plugin

```bash
make plugin
```

Produces `llvm_tm_plugin/bin/libTMInstrument.so`.

### 2. Build a specific backend

```bash
make benchmarks BACKEND=tl2              # Build all benchmarks with TL2
make benchmarks BACKEND=norec            # Build all benchmarks with NOrec
make benchmarks BACKEND=tinystm          # Build all benchmarks with TinySTM
make benchmarks BACKEND=singlelock       # Build all benchmarks with SingleGlobalLock
```

The default backend is **tinystm**. Supported values:
`singlelock`, `norec`, `tl2`, `tinystm`, `swiss`.

### 3. Build individual benchmarks

Each benchmark directory has its own Makefile with targets named
`<name>_<backend>`:

```bash
make -C plugin-benchmarks/bank bank_norec bank_tl2 bank_singlelock bank_tinystm
make -C plugin-benchmarks/datastructures avltree_NOrec avltree_SingleGlobalLock
make -C plugin-benchmarks/STMbench7 stmbench_singlelock stmbench_tl2 stmbench_tinystm
make -C plugin-benchmarks/STAMP stamp_tinystm
make -C plugin-benchmarks/ycsb ycsb_singlelock
make -C plugin-benchmarks/eigenbench eigen_singlelock
```

### 4. Build backends unit tests

```bash
make -C tests/backends all
make -C tests/backends run                    # Run all tests
```

## The Compilation Pipeline

The plugin transforms a source file into an instrumented binary in 4 steps,
automated by `llvm_tm_plugin/tm_pipeline.mk`:

```
Step 1: clang++ -O1 -fno-inline -emit-llvm -c file.cpp → file.bc
    Compile to LLVM bitcode (with -fno-inline to preserve annotations).

Step 2: opt -load-pass-plugin=libTMInstrument.so \
           -passes="tm-instrument-inline" file.bc → file.instr.bc
    TM instrumentation: replace loads/stores to TM globals with
    tm_read_*/tm_write_* calls, wrap TX functions with tm_begin/tm_end,
    clone reachable callees, redirect calls.
    Default pipeline is tm-instrument-inline; use TM_INSTRUMENT_PIPELINE
    to select tm-instrument (debug-friendly, preserves clones as separate
    functions) or tm-instrument-then-inline (instrument clones individually
    before inlining).
    Optional: -tm-opaque-symbols-file=<path> writes unresolved system
    function calls to a file for external resolution.

Step 3: opt -O3 file.instr.bc → file.opt.bc
    Optimize the instrumented IR (inlines TM runtime hooks).
    Override with TM_OPT_LEVEL=-O0 for debugging.

Step 4: clang++ file.opt.bc <runtime>.cpp → binary
    Link with the chosen backend runtime and system libraries (-lm, etc.).
```

### Debug builds

```sh
# Debug mode: noinline pipeline, -O0 post-opt, debug symbols, verbose plugin output
BUILD_TYPE=DEBUG make test_foo

# Or select pipeline manually (clone functions survive for breakpoints):
TM_INSTRUMENT_PIPELINE=tm-instrument make test_foo
```

See `llvm_tm_plugin/DEBUG.md` for the complete debugging guide (GDB setup,
symbol naming, intermediate IR inspection, pipeline comparison).

| Pipeline | Clones survive? | TM ops | Best for |
|----------|----------------|--------|----------|
| `tm-instrument-inline` (default) | No — inlined | 176 | Production: inlined code maximizes optimization |
| `tm-instrument` | Yes — `NoInline`+`OptimizeNone` | 38 | Debugging: clone functions are breakpoint-able |
| `tm-instrument-then-inline` | No — inlined | 204 | Experiment: pre-inline clone instrumentation |

### Opaque Symbol Resolution

Some standard library functions (e.g., `sqrt`, `cos`, `sin`, `pow`) are called
inside transactions but have no LLVM IR body — they are "opaque" to the plugin.
By default the plugin rejects these. To allow them:

1. Use `-tm-allow-opaque` during instrumentation to emit opaque calls as-is.
2. Use `-tm-opaque-symbols-file=<path>` to dump unresolved symbols.
3. Run `tm-resolve-opaque.py --symbols <file>` to locate symbols in system
   libraries and generate LLVM IR stub declarations.
4. Link the resulting `tm-opaque-resolved.bc` into the pipeline.

The `tm-resolve-opaque.py` tool searches system shared libraries via `nm -D`
and produces bitcode stub declarations for known functions (math, libc, pthread,
syscalls). Common math functions are handled automatically without library search.

### Makefile helpers (tm_pipeline.mk)

The shared Makefile include at `llvm_tm_plugin/tm_pipeline.mk` provides:

| Function / Variable | Purpose |
|---------------------|---------|
| `$(call tm_compile_ir,src,out)` | Step 1: `.cpp` → `.bc` |
| `$(call tm_instrument,in,out)` | Step 2: `.bc` → `.instr.bc` (uses `$(TM_INSTRUMENT_PIPELINE)`) |
| `$(call tm_optimize,in,out)` | Step 3: `.instr.bc` → `.opt.bc` (uses `$(TM_OPT_LEVEL)`) |
| `$(call tm_resolve_opaque,symbol_file)` | Resolve opaque symbols → `.bc` stubs |
| `$(call tm_link,opt_bc,backend,out)` | Step 4: `.opt.bc` + runtime → binary |
| `$(call tm_target,name,src,backend)` | Define a complete build target |
| `TM_INSTRUMENT_PIPELINE` | Pipeline name: `tm-instrument-inline` (default), `tm-instrument`, or `tm-instrument-then-inline` |
| `TM_OPT_LEVEL` | Post-instrumentation opt level: `-O3` (default) or `-O0` (debug) |
| `TM_LINK_OPT` | Link-time optimization: `-O1` (default) or `-O0 -g` (debug) |
| `BUILD_TYPE` | `RELEASE` (default) or `DEBUG` — sets pipeline, opt level, and link flags together |
| `TM_OPAQUE_SYMBOLS_FILE` | If set, passed to plugin for symbol dump |
| `TM_OPAQUE_STUBS` | Path to resolved opaque stub `.bc` |
| `TM_LINK_LIBS` | Extra libs for final link (e.g., `-lm`) |
| `TM_RESOLVE_OPAQUE` | Path to resolve script |
| `TM_INSTRUMENT_FLAGS` | Extra flags for opt pass (e.g., `-tm-allow-opaque`) |

Example usage with opaque function resolution:

```makefile
include ../../llvm_tm_plugin/tm_pipeline.mk

SRC := mybench.cpp
TM_OPAQUE_SYMBOLS_FILE = out/mybench_opaque.txt
TM_INSTRUMENT_FLAGS = -tm-allow-opaque
TM_LINK_LIBS = -lm

# Custom target with opaque resolution
bin/mybench: out/mybench.opt.bc
	$(call tm_instrument,out/mybench.bc,out/mybench.instr.bc)
	$(call tm_resolve_opaque,$(TM_OPAQUE_SYMBOLS_FILE))
	$(call tm_link,$<,tinystm,$@)

# Or use the convenience target (opaque stubs linked automatically if present):
$(eval $(call tm_target, mybench, $(SRC), singlelock))
```

## Running Benchmarks

### Benchmark runner script

```bash
./run_benchmarks.sh fast       # Quick smoke test (3 samples, light params)
./run_benchmarks.sh standard   # Standard run   (10 samples, realistic params)
./run_benchmarks.sh            # Default: standard
```

Results go to `benchmark_results/<mode>_<timestamp>/` with a `SUMMARY.txt`.

### Running individual benchmarks

```bash
# Bank (money conservation test)
plugin-benchmarks/bank/bin/bank_singlelock -t 4 -a 256 -d 3000 -r 10 -w 0
#   -t threads  -a accounts  -d duration_ms  -r %read-all  -w %write-all

# Data structures
plugin-benchmarks/datastructures/bin/avltree_SingleGlobalLock 4 10000 3000 80 10 10
#   threads  init_size  duration_ms  %read  %insert  %remove

# STMbench7 (complex CAD/CAM graph)
plugin-benchmarks/stmbench7/bin/stmbench_singlelock -t 4 -d 3000 -w 1
#   -t threads  -d duration_ms  -w workload(1=90%read/10%write)

# STAMP (Stanford TM benchmarks, TinySTM only)
plugin-benchmarks/STAMP/bin/stamp_tinystm -t 2 -d 5000 -b genome
#   -t threads  -d duration_ms  -b benchmark

# YCSB (cloud serving benchmark)
plugin-benchmarks/ycsb/bin/ycsb_singlelock -t 4 -d 3000 -w A -k 10000 -i 1000
#   -t threads  -d ms  -w workload  -k key_range  -i initial_records

# EigenBench (synthetic TM characterization)
plugin-benchmarks/eigenbench/bin/eigen_singlelock -t 2 -d 2000
```

## Assessing Instrumentation Overhead

```bash
./assess_instrumentation.sh              # All benchmarks
./assess_instrumentation.sh bank stmbench7  # Specific benchmarks
```

Produces a LaTeX table with IR line counts, TM hook counts, clone counts, etc.

## Programming Model

```cpp
#define TM  __attribute__((annotate("tm")))
#define TX  __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))
#define TM_LOCAL __attribute__((annotate("tm_local")))

TM int counter = 0;

TX void increment() {
    counter++;
}

int main() {
    increment();
}
```

### Step-by-step user workflow

| Step | What to do | Why |
|------|-----------|-----|
| 1 | Declare shared globals with `TM` | Plugin instruments every load/store to these variables |
| 2 | Mark TX functions with `TX` | Plugin wraps them with `tm_begin`/`tm_end`, replaces loads/stores with `tm_read`/`tm_write` |
| 3 | Mark thread entry points with `THREAD` | Plugin injects `tm_init_thread`/`tm_exit_thread`. Auto-detection also works for `pthread_create`, `std::thread`, OpenMP, and TBB patterns |
| 4 | `main` is auto-detected | Plugin injects `tm_init`/`tm_exit` automatically — no annotation needed |
| 5 | (Optional) Annotate private locals with `TM_LOCAL` | Skips expensive TM instrumentation on known-private stack variables |
| 6 | Link with a backend runtime | Pick a `.cpp` from `backends/runtimes/` (e.g., `TinySTM_runtime.cpp`) |

### How nesting works

When a `TX` function calls another `TX` function, the plugin **flattens** the nesting:

```
Outer TX function
  ├─ tm_begin() called here ── counter=1 → outermost
  ├─ tm_read/tm_write         ← all loads/stores instrumented
  ├─ calls inner TX function
  │   └─ counter incremented (skips tm_begin)  ← nested: no tm_begin
  │      └─ tm_read/tm_write  ← still instrumented
  └─ tm_end() called here    ── counter=1 → outermost
```

Only the outermost transaction calls `tm_begin`/`tm_end`. Nested calls skip them entirely — the hook counter (`tm_nested_call_counter`) handles this transparently. On abort (via `sigsetjmp`/`siglongjmp`), execution restarts from the outermost `tm_begin`.

### Optimizing with `tm_local`

Every load/store inside a TX function is instrumented by default. For thread-private locals that never escape, annotation eliminates the overhead:

```cpp
TX void worker() {
    TM_LOCAL int i;
    for (i = 0; i < 1000; i++) {  // i is plain load/store
        g_shared_counter++;        // g_shared_counter is tm_read/tm_write
    }
}
```

Only `__attribute__((annotate("tm_local")))` on stack **variables** (not types). Pointers to shared memory must NOT be marked `tm_local`.

### Thread entry auto-detection

In addition to the `THREAD` annotation, the plugin auto-detects common thread-entry patterns by symbol name:

| Pattern | Example symbols |
|---------|----------------|
| `pthread_create` | `pthread_create`, `start_thread` |
| `std::thread` | `std::thread::_State_impl`, `execute_native_thread_routine` |
| OpenMP | `__kmp_launch_thread`, `__kmp_fork_call` |
| TBB | `tbb::internal::start_thread` |

Any function that matches one of these patterns OR has the `THREAD` annotation gets `tm_init_thread`/`tm_exit_thread` injected.

### Annotation Reference

| Annotation | Purpose |
|------------|---------|
| `TM` | Variable is managed by the STM runtime (every load/store instrumented) |
| `TX` | Function executes as a transaction (wrapped with `tm_begin`/`tm_end`) |
| `THREAD` | Thread entry point (gets `tm_init_thread`/`tm_exit_thread`) |
| `MAIN` | Program entry point (gets `tm_init`/`tm_exit`) |
| `TM_LOCAL` | Stack variable is thread-private — skip TM instrumentation |
| `PSTATIC_REBUILD` | Called automatically after `tm_init()` restores persistent data. Used with `TX` to rebuild data structures from TM-backed arrays. |

## Explicit TM API (No Plugin Required)

For projects that cannot or should not use an LLVM plugin, the `expli_tm_api/`
directory provides a header-only C++ API that wraps the STM runtime directly.
No compiler instrumentation needed — just `#include "tm_api.hpp"` and link a
backend runtime.

### Quick Start

```cpp
#include "expli_tm_api/tm_api.hpp"
#include <cstdio>

int main() {
    expli::TM<int>::init();
    expli::TM<int>::thread_init();

    expli::TM<int> x;
    x.poke(0);

    expli::TM<int>::begin();
    x.write(42);
    int r = x.read();                       // sees own write
    expli::TM<int>::end();

    printf("%d\n", x.peek());               // 42 (committed)
    expli::TM<int>::exit();
}
```

Compile:
```bash
c++ -std=c++20 -O2 -pthread -I/path/to/tm_api_cpp  \
    -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL           \
    my_prog.cpp backends/runtimes/TinySTM_runtime.cpp -pthread
```

### Core Types

| Type | Purpose |
|------|---------|
| `TM<T>` | Transactional wrapper for a value of type `T`. Reads/writes go through the STM runtime. |
| `TM<T*>` | Pointer specialization. Manages a TM-tracked pointer + optional buffer. |
| `my::vector<T>` | `std::vector`-like container (uses `TM<T>::malloc`/`free` internally). |
| `my::pair<A,B>` | Lightweight pair. |
| `flat_map<K,V>` | Sorted-vector map with binary search. |
| `flat_multimap<K,V>` | Sorted-vector multimap. |
| `flat_set<K>` | Sorted-vector set. |

### Lifecycle

```cpp
TM<int>::init();            // one-time global init
TM<int>::thread_init();     // per-thread (before any begin)
TM<int>::begin();           // start transaction
// ... reads/writes ...
TM<int>::end();             // commit
TM<int>::thread_exit();     // per-thread cleanup
TM<int>::exit();            // global cleanup
```

### `TM<T>` methods

| Method | Inside TX | Outside TX |
|--------|-----------|-----------|
| `v.read()` | TM read (sees write-set) | — |
| `v.write(val)` | TM write (write-set) | — |
| `v.peek()` | — | Direct read of `value_` |
| `v.poke(val)` | — | Direct write to `value_` |
| `T::begin()` / `T::end()` | Nesting-aware TX control | — |
| `T::malloc(s)` / `T::free(p)` | TM-tracked alloc/free | Regular alloc/free |

### `TM<T*>` — Pointer & Array Management

Manages a TM-tracked pointer (the address is protected). The pointed-to buffer
is managed separately. Element access can go through static helpers or through
nested `TM<TM<T>*>` for `operator[]` syntax.

**Buffer management** (uses `::operator new`/`delete` — no spec_alloc tracking):
```cpp
TM<int*> buf;
buf.alloc(100);                 // ::operator new(100 * sizeof(int))
buf.free_ptr();                 // ::operator delete + set nullptr
```

**Element access (static helpers):**
```cpp
TM<int>::begin();
TM<int>::write_at(&buf.read()[i], 42);
int x = TM<int>::read_at(&buf.read()[i]);
TM<int>::end();
```

**Nicer syntax via `TM<TM<T>*>`** (each element is a `TM<T>` object):
```cpp
TM<TM<int>*> buf;
buf.alloc(100);
TM<int>::begin();
buf[i].write(42);               // operator[] returns TM<int>&
int x = buf[i].read();          // read via TM
TM<int>::end();
buf.free_ptr();
```

Does **not** compile for `TM<void*>` (void& is ill-formed).

### Data Structures

`my::vector<T>` uses `TM<T>::malloc`/`free` internally, so its buffer is
TM-tracked (spec_alloc) when allocated inside a TX, and regular heap when
allocated outside:

```cpp
TM<int>::begin();
my::vector<int> v;
v.push_back(42);                // buffer allocated via TM<int>::malloc
int x = v[0];                   // raw read (not TM-tracked — for demo only)
TM<int>::end();
```

`flat_map`, `flat_multimap`, `flat_set` provide sorted-vector containers
suitable for TM workloads (no pointer-based data structures like red-black trees).

### Known Limitations

- **Single-thread correctness only** for the WBCTL backend when the runtime
  lacks `sigsetjmp`/`siglongjmp` retry. The explicit API relies on the
  backend runtime's `tm_begin`/`tm_end` — if the runtime does not set up a
  `sigsetjmp`, TX abort will `siglongjmp` to uninitalized state. Multi-threaded
  use requires a backend that handles retry in `tm_begin` (e.g., `SingleGlobalLock`).
- `tm_realloc` has a spec_alloc tracking limitation — use `malloc`+`memcpy`+`free` instead.

### Test Suite

```bash
make -C expli-benchmarks test         # 207 ds tests + 114 tx tests
make -C expli-benchmarks run-tests    # same
```

## Backend Reference

| Backend | Build suffix | Strategy | Correctness | Speed |
|---------|-------------|----------|-------------|-------|
| SingleGlobalLock | `_singlelock` | Global `std::mutex` | Always | Fastest |
| NOrec | `_norec` | Value-based validation | Always | Fast (read-heavy) |
| TL2 | `_tl2` | Commit-time locking | ✅ | Fast (disjoint access) |
| TinySTM (WBCTL) | `_tinystm` | Write-back commit-time locking | ✅ | Slower (per-access logging) |
| SwissTM | `_swiss` | Hybrid lazy/pessimistic | ✅ | Slower (per-access logging) |
| PersistentSGL | `_persistentsgl` | SGL + mmap persistence | ✅ Array types | Single-process persistence |
| DistributedSGL | `_distributedsgl` | Shared mmap + 2PC | ✅ Cross-process | Inter-process sync via mmap |

All backends share the same `tm_*` hook interface. Select at link time by
linking against the appropriate `*_runtime.cpp`.

### PersistentSGL

File-backed persistence of TM globals with automatic heap allocation.

**Architecture:**
- A 64 MB mmap file (`benchmark_results/tm_persist.bin`) stores TM globals and a bump allocator heap
- Fixed-address mmap at `0x600000000000` (deterministic VA) so pointer-based data structures remain valid after restart
- `tm_malloc`/`tm_free` service TX allocations from the persistent heap; outside TX they fall back to system `malloc`/`free`
- Inside a TX function, `operator new` redirects to `tm_malloc`
- The plugin replaces `malloc`/`free`/`calloc`/`realloc` calls inside TX functions with their `tm_*` equivalents

**Simple types** (arrays, primitives) persist transparently:
```bash
rm -f benchmark_results/tm_persist.bin
./bin/prog_persistentsgl    # first run: stores initial state
./bin/prog_persistentsgl    # second run: restores previous state
```

**std::map persistence** requires `PSTATIC_REBUILD`:
```cpp
TM int      g_pkeys[1024];
TM int      g_pvals[1024];
TM int      g_pcount = 0;
static std::map<int, int> g_map;   // NOT TM — rebuilt on restart

TX PSTATIC_REBUILD void rebuild() {
    g_map.clear();
    for (int i = 0; i < g_pcount; i++)
        g_map[g_pkeys[i]] = g_pvals[i];
}
```

The plugin calls `rebuild()` automatically after `tm_init()` restores the arrays. Because it's `TX`, allocations go to the persistent heap. See `plugin-benchmarks/persistent_kv_stdmap.cpp`.

**Important:** Call `g_map.clear()` before `main` returns to avoid the `std::map` destructor accessing the unmapped mmap after `tm_exit()`.

### DistributedSGL

Multi-process distributed transactions via shared mmap with two-phase commit.

```bash
export TM_NPROCESSES=2
./bin/prog_distributedsgl &   # process 1 (blocks at barrier)
./bin/prog_distributedsgl     # process 2 (both proceed)
```

Each process:
1. Waits at a barrier until all `TM_NPROCESSES` processes have called `tm_init`.
2. On `tm_begin()`: acquires a spinlock (PREPARE) and syncs TM data from the shared mmap.
3. On `tm_end()`: writes TM data to the shared mmap, advances epoch, releases lock (COMMIT).

Data is transferred by offset-based memcpy (not shared pointers), so it works
across different virtual address spaces (ASLR-safe). The shared state file
is at `benchmark_results/tm_2pc_state.bin` (gitignored).

**Future directions — multi-machine distributed TM:**

The plugin already provides the TM instrumentation layer; extending DistributedSGL
to operate over a network instead of shared mmap would require:

1. **Socket-based two-phase commit** — replace the local spinlock + mmap exchange
   with `send()`/`recv()` messages between machines. The PREPARE phase sends
   the write-set; COMMIT sends an apply signal; ABORT discards.
2. **Serialisation** — flat buffers (e.g. `flatbuffers` or custom offset-based
   serialisation) for the TM data segment, since pointer values are
   machine-specific. The existing offset-based memcpy approach is already
   position-independent and trivially serialisable.
3. **Failure handling** — add timeouts, retry, and a failure detector so a
   crashed participant doesn't block the system.
4. **Epoch/clock sync** — replace the shared-memory epoch counter with a
   logical clock or hybrid logical clock (HLC) for ordering across machines.

**Future directions — state machine replication (SMR):**

The instrumented binary already exposes every memory operation to the runtime;
replication needs no annotation or plugin change. All logic lives in the runtime hooks:

1. **Deterministic replay** — TM instrumentation produces deterministic read/write
   sets per transaction. The same TX can be replayed on multiple replicas;
   `tm_end()`'s commit decision becomes the consensus proposal.
2. **Raft integration** — a `replicated_runtime.cpp` wraps `tm_begin()`/`tm_end()`:
   - Leader executes the transaction and proposes the resulting write-set as a
     Raft log entry.
   - Followers apply the committed write-set to their local TM state.
3. **Read-only fast path** — read-only transactions (no `tm_write` calls during
   the transaction) skip consensus entirely and are served directly by any replica.

### Event Logger Debugging

The `backends/tm_event_logger.hpp` header provides a per-thread ring-buffer
event logger for debugging TM backend crashes.  Activate with `-DTM_EVENT_LOG`:

```bash
make CXXFLAGS="-std=c++20 -O0 -pthread -g -DTM_EVENT_LOG" -C tests/backends run
```

The logger records TX begin/abort/commit, lock acquire/release, read/write-set
events, and gap checks.  On SIGSEGV, the last 512 events are dumped
automatically.  See `backends/README.md` for the full event type reference.

## Rust TM API

The `rust_tm_api/` directory provides a Rust port of the same TM backends with
a safe `transaction()` API.  All 10 backends share the same interface:

```rust
use tm::{transaction, TmCell};

let counter = TmCell::new(0);
transaction(|tx| {
    let val = tx.read(&counter);
    tx.write(&counter, val + 1);
});
```

### WriteBack — Safe Commit via Deferred Writes

In the Rust implementation, the `unsafe` inherent in TM commit is encapsulated
using a `WriteBack` enum defined in `runtime_core`:

```rust
pub enum WriteBack {
    U8(usize, u8),
    U16(usize, u16),
    U32(usize, u32),
    U64(usize, u64),
    Bytes(usize, Box<[u8]>),
}

impl WriteBack {
    /// Apply this write-back to memory.
    ///
    /// # Safety
    /// The caller must guarantee exclusive access to `addr` (the TM commit
    /// protocol provides this — locks held, read-set validated, global
    /// commit lock acquired).
    pub fn apply(self) {
        unsafe { /* ptr::write or copy_nonoverlapping */ }
    }
}
```

Each `write_word` pushes a `WriteBack` entry (instead of storing `(addr, value)`
and writing at commit).  `tm_commit()` iterates over `write_backs` and calls
`wb.apply()` — no `unsafe` blocks in commit. The `unsafe` is confined to
`WriteBack::apply()`, a small auditable function.

For write-through backends (WT, SwissTM), the same pattern applies to the undo
log: `undo_backs: Vec<WriteBack>` captures old values at write time; rollback
calls `u.apply()` safely.

### Limitations

- **TSXSGL backends** (`tsxsgl`, and by extension NVHTM/SPHT when using RTM)
  cannot use `WriteBack` for all paths — their `tm_write_*` functions do
  direct `unsafe { addr.write(val) }` because writes reach memory immediately
  under a spinlock (no buffering).
- **Write-through backends** (WT, SwissTM) still require `unsafe` blocks in
  `write_word` for the immediate memory write — the write-through itself
  cannot be deferred.
- **`TmPrimitive` trait** (`tm_read` / `tm_write`) is `unsafe fn` because it
  operates on raw pointers `*mut T`.  The safe API is `Transaction::read`
  / `Transaction::write` which takes `&TmCell<T>`.

### Performance Comparison

All tests on 2 threads, 5000ms duration (bank: 2000ms).

| Backend | EigenBench (tx/s) | Bank (tx/s) | Aborts | Type |
|---------|:-----------------:|:-----------:|:------:|:----:|
| Rust TSXSGL | 5,336,219 | — | 0 | SGL |
| Rust NOrec | 1,188,465 | — | 67K | Value-based |
| Rust TL2 | 1,009,388 | — | 77 | Commit-time |
| Rust NVHTM | 983,977 | — | 40 | Commit-time |
| Rust WBCTL | 968,246 | 430,858 | 69 | Encounter-time |
| Rust SwissTM | 899,750 | — | 13K | Write-through |
| Rust WT | 852,271 | — | — | Write-through |
| Rust SPHT | 834,213 | — | 58 | Commit-time |
| Rust WBETL | 791,849 | — | 19 | Encounter-time |
| C++ expli WBCTL | 524,527 | 130,281 | 12K | Encounter-time |
| C++ LLVM plugin | — | 36,013 | 2.7K | Plugin-instr. |

Rust is consistently 1.5–12× faster than the equivalent C++ backend due to:
lower type-erasure overhead (native enums vs `any_type_t` unions), more
efficient `HashMap` for write-sets, and aggressive monomorphization of
generic read/write paths.
