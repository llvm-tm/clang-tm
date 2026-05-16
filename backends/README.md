# STM Backends

This directory contains the Software Transactional Memory backends.
Each backend implements the `tm_read_*`/`tm_write_*` hooks that the
LLVM plugin calls from instrumented bitcode.

## Architecture

```
backends/
├── runtimes/              # Runtime wrappers (one per backend)
│   ├── SingleGlobalLock_runtime.cpp
│   ├── tl2_runtime.cpp
│   ├── NOrec_runtime.cpp
│   ├── TinySTM_runtime.cpp
│   ├── SwissTM_runtime.cpp
│   ├── PersistentSGL_runtime.cpp
│   └── DistributedSGL_runtime.cpp
├── TinySTM/               # Write-back commit-time/encounter-time + write-through
│   ├── tinystm_common.hpp
│   ├── tinystm_globals.hpp
│   ├── tinystm_wbctl.hpp     # Write-back commit-time locking
│   ├── tinystm_wbetl.hpp     # Write-back encounter-time locking
│   └── tinystm_wt.hpp        # Write-through
├── TL2/                   # Transactional Locking 2
│   └── tl2.hpp
├── NOrec/                 # Lazy value-based validation STM
│   ├── NOrec.hpp
│   └── NOrec_globals.hpp
├── SwissTM/               # Hybrid lazy/pessimistic
│   └── SwissTM.hpp
├── tests/                 # Standalone backend correctness tests
├── rel_ptr.hpp            # Relative-pointer class for ASLR-safe shared memory
├── tm_alloc_overrides.hpp # operator new/delete overrides for persistent heap
└── tm_common.hpp          # Shared type definitions
```

## Backend Selection

The backend is selected at **link time** by including the corresponding
runtime file.  All backends share the same `.opt.bc` (instrumented IR);
only the final link step differs.

| Backend            | Runtime file                         | Additional flags          | Include paths               |
|--------------------|--------------------------------------|---------------------------|-----------------------------|
| SingleGlobalLock   | `SingleGlobalLock_runtime.cpp`       | *(none)*                  | *(none)*                    |
| TL2                | `tl2_runtime.cpp`                    | *(none)*                  | `-Ibackends/TL2`           |
| NOrec              | `NOrec_runtime.cpp`                  | *(none)*                  | `-Ibackends/NOrec`         |
| TinySTM (WBCTL)    | `TinySTM_runtime.cpp`                | `-DDESIGN_WBCTL`          | `-Ibackends/TinySTM`       |
| TinySTM (WBETL)    | `TinySTM_runtime.cpp`                | `-DDESIGN_WBETL`          | `-Ibackends/TinySTM`       |
| TinySTM (WT)       | `TinySTM_runtime.cpp`                | `-DDESIGN_WT`             | `-Ibackends/TinySTM`       |
| SwissTM            | `SwissTM_runtime.cpp`                | *(none)*                  | `-Ibackends/SwissTM`       |
| PersistentSGL      | `PersistentSGL_runtime.cpp`          | *(none)*                  | *(none)*                    |
| DistributedSGL     | `DistributedSGL_runtime.cpp`         | *(none)*                  | *(none)*                    |

## Runtime API

Each runtime wrapper exports the following functions:

| Function              | Purpose                                  |
|-----------------------|------------------------------------------|
| `tm_init()`           | Global init (once per process)           |
| `tm_exit()`           | Global cleanup                           |
| `tm_init_thread()`    | Per-thread init                          |
| `tm_exit_thread()`    | Per-thread cleanup                       |
| `tm_begin()`          | Begin transaction                        |
| `tm_end()`            | End transaction                          |
| `tm_read_i{1,2,4,8}()`| Typed reads (int8/16/32/64)             |
| `tm_write_i{1,2,4,8}()`| Typed writes                           |
| `tm_read_f{4,8}()`    | Float/double reads                       |
| `tm_write_f{4,8}()`   | Float/double writes                      |
| `tm_read_ptr()`       | Pointer read                             |
| `tm_write_ptr()`      | Pointer write                            |
| `tm_set_jmpbuf()`     | Register setjmp buffer for retry         |
| `tm_longjmp()`        | Abort and retry transaction              |
| `tm_malloc()`         | Transactional allocation                 |
| `tm_free()`           | Transactional deallocation               |
| `tm_serialize_lock()` | Serialize access to a data structure     |
| `tm_serialize_unlock()`| Release serialization lock              |

## Comparison

| Backend          | Read Overhead | Write Overhead | Complexity | Speed       |
|------------------|---------------|----------------|------------|-------------|
| SingleGlobalLock | None          | None (serial) | Lowest     | Fastest     |
| NOrec            | Low (clock)   | Medium (CAS)   | Low        | Fast        |
| TL2              | Medium (ver)  | High (lock)    | Medium     | Fast (2T)   |
| TinySTM          | High (log)    | High (log+lock)| High       | Slow w/ inst|
| SwissTM          | High (lazy)   | High (per-access)| High      | Slow        |

## Running Backend Tests

The `tests/` subdirectory contains standalone correctness tests that
compile directly against each backend (no LLVM plugin needed).

```
cd backends/tests
make run
```

Available targets:

| Target                     | Description                              |
|----------------------------|------------------------------------------|
| `make all`                 | Build all backend tests                  |
| `make run`                 | Build and run TinySTM flavor tests       |
| `make test_tl2_simple`     | TL2 simple correctness test              |
| `make test_tl2_multi`      | TL2 multi-threaded test                  |
| `make test_NOrec_counter`  | NOrec counter test                       |
| `make runtime__st`         | TinySTM single-thread test               |
| `make runtime__mt`         | TinySTM multi-thread test                |
| `make tinystm_all`         | All three TinySTM flavors (WBCTL/WBETL/WT) |
| `make clean`               | Remove all build artifacts               |

Each test verifies basic TM operations (reads, writes, atomicity) and
reports `PASS`/`FAIL` on exit.
