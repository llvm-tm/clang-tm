# STM Backends

This directory contains the Software Transactional Memory backends.
Each backend implements the `tm_read_*`/`tm_write_*` hooks that the
LLVM plugin calls from instrumented bitcode.

## Architecture

```
backends/
├── stubs/                      # Stub implementations for testing
│   ├── tm_stubs.cpp
│   └── tm_stubs.hpp
├── tm_impl/                    # TM algorithm implementations
│   ├── common/                 # Shared headers used by all backends
│   │   ├── rel_ptr.hpp
│   │   ├── tm_alloc_overrides.hpp
│   │   ├── tm_api.hpp
│   │   ├── tm_common.hpp
│   │   ├── tm_debug.hpp
│   │   ├── tm_event_logger.hpp
│   │   ├── tm_log_entries.hpp
│   │   ├── tm_log_merge.hpp
│   │   ├── tm_opaque_safe.hpp
│   │   ├── tm_perf_counters.hpp
│   │   ├── tm_platform.hpp
│   │   ├── tm_rbtree.hpp
│   │   ├── tm_region_allocator.hpp
│   │   ├── tm_safe_map.hpp
│   │   ├── tm_spin_token.hpp
│   │   └── tm_thread_state.hpp
│   ├── tiny_stm/               # Write-back commit-time + write-through
│   ├── tl2/                    # Transactional Locking 2
│   ├── norec/                  # Lazy value-based validation STM
│   ├── swisstm/                # Hybrid lazy/pessimistic
│   ├── single_global_lock/     # Single global lock (serial execution)
│   ├── tsx_sgl/                # TSX + single global lock hybrid
│   ├── persistent_sgl/         # Persistent + single global lock
│   ├── distributed_sgl/        # Distributed + single global lock
│   ├── queue/                  # Queue-based runtime
│   ├── nvhtm/                  # NV-HTM
│   ├── dudetm/                 # DudeTM
│   ├── romulus/                # Romulus Log
│   ├── leftright/              # Left-Right
│   ├── spht/                   # SPHT
│   ├── xtm/                    # XTM
│   └── tm_region_allocator/    # TM address-space region allocator
└── README.md
```

## Backend Selection

The backend is selected at **link time** by including the corresponding
runtime file.  All backends share the same `.opt.bc` (instrumented IR);
only the final link step differs.

| Backend            | Runtime file                         | Additional flags          | Include paths               |
|--------------------|--------------------------------------|---------------------------|-----------------------------|
| SingleGlobalLock   | `SingleGlobalLock_runtime.cpp`       | *(none)*                  | *(none)*                    |
| TL2                | `tl2_runtime.cpp`                    | *(none)*                  | `-Ibackends/tm_impl/tl2`           |
| NOrec              | `NOrec_runtime.cpp`                  | *(none)*                  | `-Ibackends/tm_impl/norec`         |
| TinySTM (WBCTL)    | `TinySTM_runtime.cpp`                | `-DDESIGN_WBCTL`          | `-Ibackends/tm_impl/tiny_stm`       |
| TinySTM (WBETL)    | `TinySTM_runtime.cpp`                | `-DDESIGN_WBETL`          | `-Ibackends/tm_impl/tiny_stm`       |
| TinySTM (WT)       | `TinySTM_runtime.cpp`                | `-DDESIGN_WT`             | `-Ibackends/tm_impl/tiny_stm`       |
| SwissTM            | `SwissTM_runtime.cpp`                | *(none)*                  | `-Ibackends/tm_impl/swisstm`       |
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
cd tests/backends/tm_impl
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
cd tests/backends/tm_impl
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
| `backends/tm_impl/common/tm_event_logger.hpp`    | Ring buffer + event macros  |
| `backends/tm_impl/tiny_stm/tinystm_wbctl.hpp` | Instrumented with events  |
| `backends/tm_impl/tiny_stm/tinystm_common.hpp` | SIGSEGV handler install   |
| `tools/stm_bug_tool/timeline_viz.py` | PDF timeline visualizer   |

### Timeline PDF Visualization

`timeline_viz.py` produces a PDF timeline from event logs with one lane per
thread, TX boundaries as colored bands, and invariant violations highlighted
with red rings and annotation labels above the plot.

The plot uses a dense event-index x-axis that compresses time gaps, ensuring
events are evenly spaced regardless of silent periods between clusters.

**Prerequisites:** `matplotlib` (Python package). The tool auto-detects and
reports any invariant violations before generating the timeline.

**Usage:**

```bash
# 1. Generate a raw event log from a benchmark with TM_EVENT_LOG:
cd benchmarks/plugin/stmbench7
rm -f bin/stmbench_tl2 && make stmbench_tl2 TM_DEFINES_tl2="-DTM_EVENT_LOG"
./bin/stmbench_tl2 -t 4 -d 5000 -w 1 2>/tmp/event_log.txt

# 2. Generate a timeline PDF from the log:
python3 tools/stm_bug_tool/timeline_viz.py --log /tmp/event_log.txt \
    --output timeline.pdf

# 3. Or run a benchmark live (build + run + parse + plot in one command):
python3 tools/stm_bug_tool/timeline_viz.py --backend swisstm \
    --benchmark counter --threads 4 --iters 2000 \
    --output counter_timeline.pdf

# 4. Adjust the event window around the first violation:
python3 tools/stm_bug_tool/timeline_viz.py --log /tmp/event_log.txt \
    --window 160 --output focused.pdf

# 5. Center the window on a specific event index (e.g., after all threads start):
python3 tools/stm_bug_tool/timeline_viz.py --log /tmp/event_log.txt \
    --center 35000 --window 160 --output multi_threaded.pdf

# 6. Pass --keep-bin to preserve the built binary between runs:
python3 tools/stm_bug_tool/timeline_viz.py --backend tl2 \
    --benchmark bank --threads 4 --iters 1000 \
    --keep-bin --output bank_tl2.pdf
```

**CLI options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--log PATH` | — | Event log file (overrides live run) |
| `--backend NAME` | — | Backend: `tinystm`, `swisstm`, `tl2`, `norec` |
| `--benchmark NAME` | `counter` | Benchmark: `counter`, `bank` |
| `--threads N` | `4` | Number of threads |
| `--iters N` | `2000` | Iterations per thread |
| `--counters N` | `1` | Number of counters (counter benchmark) |
| `--window N` | `200` | Number of events in the focused window |
| `--center N` | — | Center window on this event index (default: first non-warmup violation or middle of log) |
| `--keep-bin` | — | Keep built binary after run |
| `--output PATH` | `timeline.pdf` | Output PDF path |

**Backend names:** `tinystm`, `swisstm`, `tl2`, `norec`, `wt`, `wbetl`

**What the PDF shows:**
- **Lanes**: one horizontal lane per thread, labeled by thread ID
- **TX bands**: vertical spans (green=commit, red=abort) showing the lifetime of each transaction from TX_BEGIN to COMMIT_SUCCESS/TX_ABORT. The band spans events from begin to end on that thread's lane — wide bands mean the TX was long (many interleaved events from other threads)
- **Markers**: color-coded by event type, shape-coded by role:
  - Pentagons: TX_BEGIN / TX_ABORT
  - Circles: COMMIT_SUCCESS / COMMIT_LOCK_ACQUIRE
  - Triangles up: READ_LOCK_ACQUIRE / READ_VERSION_CHECK (reads)
  - Triangles down: WRITE_SET_INSERT / WRITE_LOCK_ACQUIRE / COMMIT_WRITEBACK (writes)
- **Violations**: red ring (same shape as base marker) + cross, vertical dotted line to top label
- **Labels**: rotated 90° description text above the plot for each violation
- **Footer**: event range, TX count, commit/abort/violation counts
- **Dual x-axis**: top axis shows timestamps (μs), bottom shows event sequence
