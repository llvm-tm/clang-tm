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

## Event Logger

The file `tm_event_logger.hpp` provides a per-thread ring-buffer event
logger for debugging TM backend crashes (e.g., the WBCTL SIGSEGV at
4 threads).  It is activated by defining `TM_EVENT_LOG` before including
the header; when inactive, all event macros compile to nothing.

### Usage

```cpp
#define TM_EVENT_LOG                        // enable (must be before includes)
#include "tm_event_logger.hpp"

TM_EVENT(TX_BEGIN, tx->id, tx->start_version);
TM_EVENT2(READ_LOCK_ACQUIRE, addr, lock, version);

TM_EVENT_DUMP(256);                         // dump last 256 events per thread
TM_EVENT_INSTALL_SIGSEGV();                 // auto-dump on crash
```

Events are logged to a lock-free per-thread ring buffer (16384 entries).
On SIGSEGV (or by calling `dump()`), the ring buffer is printed to stderr
with timestamps, thread IDs, and event payloads.

### Event Types

| Event                 | `addr1`      | `addr2`       | `data`            |
|-----------------------|--------------|---------------|-------------------|
| `TX_BEGIN`            | tx id        | —             | start_version     |
| `TX_END`              | tx id        | —             | —                 |
| `TX_ABORT`            | tx id        | —             | abort_count       |
| `READ_LOCK_ACQUIRE`   | addr         | lock          | version           |
| `READ_VERSION_CHECK`  | addr         | lock          | version           |
| `WRITE_LOCK_ACQUIRE`  | addr         | lock          | lock_state        |
| `WRITE_SET_INSERT`    | addr         | lock          | type_size         |
| `COMMIT_LOCK_ACQUIRE` | lock         | addr          | type_size         |
| `COMMIT_WRITEBACK`    | tx id        | —             | write_set_size    |
| `COMMIT_SUCCESS`      | tx id        | —             | commit_version    |
| `GAP_CHECK`           | tx id        | end_version   | commit_version    |
| `LOCK_RELEASE`        | lock         | addr          | commit_version    |
| `RETRY_END`           | tx id        | —             | abort_count       |

### Activating for WBCTL Debugging

```bash
# Add -DTM_EVENT_LOG to CXXFLAGS when compiling the runtime:
cd backends/tests
make CXXFLAGS="-std=c++20 -O0 -pthread -g -DTM_EVENT_LOG" run
```

The SIGSEGV handler is installed automatically by `tinystm::init()` when
`TM_EVENT_LOG` is defined.  To dump events on demand from lldb:

```
p ((stm::EventRing*)&stm::get_event_ring())->dump(0, stderr)
```

### Files

| File                              | Role                        |
|-----------------------------------|-----------------------------|
| `backends/tm_event_logger.hpp`    | Ring buffer + event macros  |
| `backends/TinySTM/tinystm_wbctl.hpp` | Instrumented with events  |
| `backends/TinySTM/tinystm_common.hpp` | SIGSEGV handler install   |
