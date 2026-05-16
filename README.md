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

- **LLVM 16+** with the `opt` tool and development libraries
- **Clang 16+** (matching LLVM version)
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
make -C benchmarks/test/bank bank_norec bank_tl2 bank_singlelock bank_tinystm
make -C benchmarks/datastructures avltree_NOrec avltree_SingleGlobalLock
make -C benchmarks/STMbench7 stmbench_singlelock stmbench_tl2 stmbench_tinystm
make -C benchmarks/STAMP stamp_tinystm
make -C benchmarks/YCSB ycsb_singlelock
make -C benchmarks/EigenBench eigen_singlelock
```

### 4. Build backends unit tests

```bash
make -C backends/tests all
make -C backends/tests run                    # Run all tests
make -C backends/tests test_tl2_simple         # Single test
make -C backends/tests tinystm_all             # All 3 TinySTM flavors
```

## The Compilation Pipeline

The plugin transforms a source file into an instrumented binary in 4 steps,
automated by `llvm_tm_plugin/tm_pipeline.mk`:

```
Step 1: clang++ -O3 -fno-inline -emit-llvm -c file.cpp → file.bc
    Compile to LLVM bitcode (with -fno-inline to preserve annotations).

Step 2: opt -load-pass-plugin=libTMInstrument.so \
           -passes="tm-instrument" file.bc → file.instr.bc
    TM instrumentation: replace loads/stores to TM globals with
    tm_read_*/tm_write_* calls, wrap TX functions with tm_begin/tm_end,
    clone reachable callees, redirect calls.

Step 3: opt -O3 file.instr.bc → file.opt.bc
    Optimize the instrumented IR (inlines TM runtime hooks).

Step 4: clang++ file.opt.bc <runtime>.cpp → binary
    Link with the chosen backend runtime.
```

### Makefile helpers (tm_pipeline.mk)

The shared Makefile include at `llvm_tm_plugin/tm_pipeline.mk` provides:

| Function | Purpose |
|----------|---------|
| `$(call tm_compile_ir,src,out)` | Step 1: `.cpp` → `.bc` |
| `$(call tm_instrument,in,out)` | Step 2: `.bc` → `.instr.bc` |
| `$(call tm_optimize,in,out)` | Step 3: `.instr.bc` → `.opt.bc` |
| `$(call tm_link,opt_bc,backend,out)` | Step 4: `.opt.bc` + runtime → binary |
| `$(call tm_target,name,src,backend)` | Define a complete build target (`name_backend`) |

Example usage in a benchmark Makefile:

```makefile
include ../../llvm_tm_plugin/tm_pipeline.mk

SRC := mybench.cpp
$(eval $(call tm_target, mybench, $(SRC), singlelock))
$(eval $(call tm_target, mybench, $(SRC), tinystm))
$(eval $(call tm_target, mybench, $(SRC), tl2))
```

This creates targets: `mybench_singlelock`, `mybench_tinystm`, `mybench_tl2`.

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
benchmarks/test/bank/bin/bank_singlelock -t 4 -a 256 -d 3000 -r 10 -w 0
#   -t threads  -a accounts  -d duration_ms  -r %read-all  -w %write-all

# Data structures
benchmarks/datastructures/bin/avltree_SingleGlobalLock 4 10000 3000 80 10 10
#   threads  init_size  duration_ms  %read  %insert  %remove

# STMbench7 (complex CAD/CAM graph)
benchmarks/STMbench7/bin/stmbench_singlelock -t 4 -d 3000 -w 1
#   -t threads  -d duration_ms  -w workload(1=90%read/10%write)

# STAMP (Stanford TM benchmarks, TinySTM only)
benchmarks/STAMP/bin/stamp_tinystm -t 2 -d 5000 -b genome
#   -t threads  -d duration_ms  -b benchmark

# YCSB (cloud serving benchmark)
benchmarks/YCSB/bin/ycsb_singlelock -t 4 -d 3000 -w A -k 10000 -i 1000
#   -t threads  -d ms  -w workload  -k key_range  -i initial_records

# EigenBench (synthetic TM characterization)
benchmarks/EigenBench/bin/eigen_singlelock -t 2 -d 2000
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

TM int counter = 0;

TX void increment() {
    counter++;
}

int main() {
    increment();
}
```

| Annotation | Purpose |
|------------|---------|
| `TM` | Variable is managed by the STM runtime (every load/store instrumented) |
| `TX` | Function executes as a transaction (wrapped with `tm_begin`/`tm_end`) |
| `THREAD` | Thread entry point (gets `tm_init_thread`/`tm_exit_thread`) |
| `MAIN` | Program entry point (gets `tm_init`/`tm_exit`) |
| `PSTATIC_REBUILD` | Called automatically after `tm_init()` restores persistent data. Used with `TX` to rebuild data structures from TM-backed arrays. |

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

The plugin calls `rebuild()` automatically after `tm_init()` restores the arrays. Because it's `TX`, allocations go to the persistent heap. See `benchmarks/persistent_kv_stdmap.cpp`.

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

The plugin can be used to build a replicated state machine where each replica
runs the same TM workload and decisions are agreed via consensus:

1. **Deterministic replay** — since TM instrumentation produces deterministic
   read/write sets, the same transaction can be replayed on multiple replicas.
   The `tm_end()` commit decision (go/no-go) becomes the consensus proposal.
2. **Raft integration** — wrap `tm_begin()`/`tm_end()` in a Raft state machine:
   - Leader executes the transaction and proposes the resulting write-set as a
     log entry.
   - Followers apply the committed write-set to their local TM state.
3. **Read-only fast path** — read-only transactions (no `tm_write` calls during
   the transaction) can be served directly by any replica without going through
   consensus, since they don't modify state.
4. **Plugin annotation** — add a `REPLICATED` annotation (`__attribute__((annotate("replicated")))`)
   so the plugin automatically routes TM operations through the consensus layer
   without manual wiring.
