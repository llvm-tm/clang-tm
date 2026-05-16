# STM Runtimes

Each runtime is a self-contained `.cpp` file that implements the TM
hook functions called by the LLVM plugin's instrumented bitcode.
At link time, exactly one runtime file is compiled together with the
`.opt.bc` to produce the final executable.

## Runtimes

### SingleGlobalLock (`SingleGlobalLock_runtime.cpp`)

The simplest possible backend.  A single `std::mutex` protects every
transaction.  Because the lock grants exclusive access, `tm_read_*` and
`tm_write_*` hooks are trivial (they just read/write memory directly).
No contention management, no retry, no read-set or write-set logging.

Use for: correctness baseline, maximum throughput with non-conflicting
workloads, scalability assessment.

### TL2 (`tl2_runtime.cpp`)

Transactional Locking 2.  Each address has a versioned lock (lock bit +
version number).  Reads acquire no lock (they just check the version);
writes acquire the address's lock at commit time.  On commit, the write
set is locked, the read set is validated, then new versions are written.

Use for: medium-contention workloads; generally the best performance
among the academic STM backends in this project.

### NOrec (`NOrec_runtime.cpp`)

Lazy value-based validation STM.  Uses a global clock and value-based
validation: on commit, the read-set is re-read and compared against the
values seen during the transaction.  No per-address locking.

Use for: low-contention read-dominated workloads.

### TinySTM (`TinySTM_runtime.cpp`)

Three variants controlled by `-DDESIGN_*`:

| Macro           | Strategy                                    |
|-----------------|---------------------------------------------|
| `DESIGN_WBCTL`  | Write-back commit-time locking (default)    |
| `DESIGN_WBETL`  | Write-back encounter-time locking           |
| `DESIGN_WT`     | Write-through                               |

TinySTM maintains a read-set and write-set per transaction.  Reads log
the address and value; writes buffer locally.  On commit, the write-set
locks are acquired, the read-set is validated, then buffered writes are
flushed to memory.

Use for: high-contention workloads where per-access logging is acceptable.

### SwissTM (`SwissTM_runtime.cpp`)

Hybrid lazy/pessimistic STM.  Combines lazy conflict detection for
reads with pessimistic detection for writes.  Higher overhead than
TL2 but can excel on specific access patterns.

### PersistentSGL (`PersistentSGL_runtime.cpp`)

A **showcase** backend — not a production STM.  Extends
SingleGlobalLock with a 64 MB bump allocator inside an mmap'd file.
`tm_malloc` allocates from this persistent region; `tm_exit` saves TM
symbol state to the file so data survives process restarts.

Requires `PSTATIC_REBUILD` functions to restore pointer-based data
structures (e.g. `std::map`) after `tm_init()` reloads the symbol
state.  See `docs/Allocators.md` for details.

### DistributedSGL (`DistributedSGL_runtime.cpp`)

A **showcase** backend — not a production STM.  Simulates a distributed
transaction system by running N processes that share state through an
mmap'd file.  Uses a two-phase commit protocol (PREPARE → sync → COMMIT)
with spinlock synchronisation and epoch counters.

Usage:
```
export TM_NPROCESSES=2
./bin/prog &   # process 1
./bin/prog     # process 2
```

## Building an Application with the Plugin + Runtime

The canonical build pipeline is:

```
source.cpp
    │ clang++ -emit-llvm
    ▼
source.bc
    │ opt -load-pass-plugin=libTMInstrument.so -passes=tm-instrument
    ▼
source.instr.bc
    │ opt -O3
    ▼
source.opt.bc
    │ clang++ ... <runtime>.cpp
    ▼
binary (executable)
```

The `tm_pipeline.mk` include file automates this.  A minimal Makefile:

```makefile
include path/to/tm_pipeline.mk
$(eval $(call tm_define_rules))

all: myapp_singlelock

$(eval $(call tm_target, myapp_singlelock, myapp.cpp, singlelock))
```

Then `make` builds `bin/myapp_singlelock`.

To select a different backend, change the third argument:

```makefile
$(eval $(call tm_target, myapp_tl2,     myapp.cpp, tl2))
$(eval $(call tm_target, myapp_norec,   myapp.cpp, norec))
$(eval $(call tm_target, myapp_tinystm, myapp.cpp, tinystm))
```

### Manual build (without tm_pipeline.mk)

```sh
# Step 1: compile to LLVM IR
clang++ -std=c++20 -O3 -fno-inline -emit-llvm -c myapp.cpp -o out/myapp.bc

# Step 2: instrument with the TM plugin
opt -load-pass-plugin=bin/libTMInstrument.so \
    -passes="tm-instrument" out/myapp.bc -o out/myapp.instr.bc

# Step 3: optimise the instrumented IR
opt -O3 out/myapp.instr.bc -o out/myapp.opt.bc

# Step 4: link with a runtime
clang++ -std=c++20 -O3 out/myapp.opt.bc \
    backends/runtimes/TinySTM_runtime.cpp \
    -DDESIGN_WBCTL -Ibackends/TinySTM -Ibackends \
    -o bin/myapp_tinystm
```

## Thread-Local Variables

Every runtime must define these thread-local variables (required by the
plugin's IR):

```cpp
__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
```

When building with a plugin that has `DISABLE_SETJMP`, the runtime
still provides these symbols (they are just unused).

## Transactional Allocator Overrides

All runtimes include `tm_alloc_overrides.hpp`, which overrides all 16
C++ `operator new`/`delete` variants.  When `g_in_tx` is `true`, they
dispatch to `tm_malloc`/`tm_free`; otherwise they fall through to the
system allocator.  Each runtime file must define `thread_local bool g_in_tx`
before including the overrides header.
