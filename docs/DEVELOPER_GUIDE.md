# Developer Guide — TM API C++

## Project Overview

The TM API C++ project implements Software Transactional Memory (STM) across 15 backends, supports two instrumentation pipelines (explicit API and LLVM plugin), has a Rust deterministic simulator, and provides benchmark suites for evaluation.

## Directory Layout

```
├── backends/tm_impl/         # C++ TM backend implementations (15 backends)
│   ├── common/               # Shared: tm_hooks, tm_api, allocators, debug, trace
│   ├── tm_region_allocator/  # mmap-based TM region allocator
│   └── <backend>/            # Individual backends (norec, tl2, tinystm, swisstm, ...)
├── benchmarks/
│   ├── cpp/                  # Explicit API benchmarks (bank, STAMP, stmbench7, ...)
│   ├── plugin/               # Plugin benchmarks (LLVM-pass instrumented)
│   └── rust/                 # Rust benchmarks (Cargo workspace)
├── expli_instr/              # Explicit instrumentation (no LLVM plugin)
│   ├── cpp/include/          # C++ API: tm_api.hpp, containers, tx_executor, etc.
│   └── rust/workspace/       # Rust API: tm crate + 14 runtime backends + simulator
├── plugin/                   # LLVM TM plugin (passes, runtimes, race checker)
│   ├── passes/               # 5-pass Honorio pipeline
│   ├── runtime/              # tm_runtime.cpp, persistent.cpp
│   └── bin/                  # libTMInstrument.so, libTMRaceChecker.so
├── simulator/                # Deterministic discrete-event simulator (Rust)
│   └── src/                  # Engine, model, backend abstraction, binaries
├── tests/                    # Test suites (expli-api, plugin, backends)
├── docs/                     # Documentation
├── patches/                  # Debug and profiling patches
└── .github/workflows/        # CI configuration (6 jobs)
```

## Build Systems

### Top-level Makefile

```
make all                        # Build plugin + plugin-benchmarks + expli-benchmarks
make plugin                     # Build libTMInstrument.so
make -C benchmarks/cpp all      # Build all expli benchmarks (BACKEND=tinystm default)
make -C plugin run              # Build and run plugin tests
make check-all                  # Build+test all 13 C++ backends
```

Backend selection: `BACKEND={TINYSTM,WBETL,WT,NOREC,SWISSTM,TL2,SGL,XTM,LEFTRIGHT,ROMULUS,SPHT,TSXSGL}`

### Explicit API (no LLVM needed)

```
make -C benchmarks/cpp bin/test_tx BACKEND=NOREC
./benchmarks/cpp/bin/test_tx    # 114 tests
make -C benchmarks/cpp bin/test_ds BACKEND=NOREC
./benchmarks/cpp/bin/test_ds    # 207 tests
```

### LLVM Plugin (requires LLVM 22)

```
make plugin                     # Build libTMInstrument.so
make -C plugin race-checker     # Build libTMRaceChecker.so
make -C plugin test             # Build all 50+ plugin tests
make -C plugin run              # Run plugin tests
```

### Rust Workspace

```
cd expli_instr/rust/workspace
cargo check --features wbctl -p tm
cargo test --features wbctl -p tm
cargo test --features simulation,norec -- --test-threads=1  # simulator tests
```

### Simulator

```
cd simulator
cargo run -- tm-sim --backend norec --trace /tmp/trace.jsonl
cargo run -- tm-gen --bank   # Generate synthetic traces
cargo test -- --test-threads=1
```

## Two Instrumentation Pipelines

### Explicit API (Expli)

Manual annotation with `TM<T>` wrappers. No LLVM dependency. Ideal for development and testing.

```cpp
#include "expli_tm_api/tm_api.hpp"
expli::TM<int> counter;
expli::TM<int>::transaction([&] {
    counter.write(counter.read() + 1);
});
```

### LLVM Plugin (Plugin)

Automatic instrumentation via LLVM pass. Uses annotations on globals and functions.

```cpp
TM int counter;
TX void increment() { counter++; }
```

Pipeline: `collect → clone → redirect → instrument-fn → cleanup`

## How to Add a C++ Backend

1. Create `backends/tm_impl/<name>/<Name>_runtime.cpp` and `<name>.hpp`
2. Implement 22+ static hook functions (begin, end, read/write for 7 types, alloc, env)
3. Build a `TMRealHooks` registration table with all function pointers
4. Register via `tm_register_real_hooks()` in `tm_init()`
5. Add `ifeq (BACKEND,<NAME>)` block in `benchmarks/cpp/Makefile`
6. Test: `make -C benchmarks/cpp bin/test_tx BACKEND=<NAME>`

## How to Add a Rust Backend

1. Create `expli_instr/rust/workspace/runtime/<name>/` with `Cargo.toml` + `src/lib.rs`
2. Export all `tm_read_*` / `tm_write_*` functions + `tm_begin`/`tm_commit`/`tm_abort`/lifecycle
3. Optionally add `pub mod sim` with simulation support
4. Register in `tm/Cargo.toml` and `tm/src/lib.rs`
5. Add exclusivity check in `tm/src/lib.rs`
6. Test: `cargo test --features <name> -p tm`

## Testing

| Test | Command | Count |
|------|---------|-------|
| Transaction correctness | `make run-test-tx BACKEND=NOREC` | 114 |
| Data structure correctness | `make run-test-ds BACKEND=NOREC` | 207 |
| All backends | `make check-all` | 13 backends × 321 each |
| Plugin tests | `make -C plugin run` | 50+ |
| Simulator tests | `cargo test -- --test-threads=1` | 26 unit + 26 integration |
| Rust workspace | `cargo test --features wbctl` | 9 |

## Debugging

- `DEBUG=1` — `-O0 -g` for all builds
- `-DTM_DEBUG_ALLOC` — track live TM allocations, detect leaks
- `-DTM_EVENT_LOG` — thread-local ring buffer with SIGSEGV handler
- `TM_TRACE_PATH=/tmp/trc.jsonl` — generate JSONL traces for simulator replay
- `patches/debug/apply.sh` — add back removed debug printfs
- `opt -passes="tm-race-checker"` — scan for missing TM annotations

## Key Patterns

- **Hooks**: All C++ backends register via `TMRealHooks` struct (22 function pointers). Hooks must be `static` to avoid TEXT-vs-DATA symbol conflicts.
- **Allocation**: `tm_region_malloc` allocates from a fixed mmap'd region. Inside transactions, `tm_track_spec_alloc` adds to the speculative allocation list (cleared on abort).
- **TLS**: Three shared TLS variables (`tm_jmpbuf`, `tm_nested_call_counter`, `tm_longjmp_ret`) defined in `tm_hooks.cpp`.
- **Simulation**: Rust backends gate thread-local state behind `#[cfg(feature = "simulation")]` — replaces `thread_local!` with `Mutex<HashMap<u64, State>>`.
- **Checkpoint/Restore**: `sim_snapshot_bytes()` / `sim_restore_bytes()` serialize per-thread backend state for deterministic replay.

## CI Pipeline (6 jobs)

| Job | What it does |
|-----|-------------|
| plugin-test | Build plugin + race checker + run plugin tests |
| simulator-test | Build simulator + run all tests + synthetic trace check |
| rust-build | Build workspace + benchmarks + run tests |
| race-checker | Run race checker on all .bc files |
| cross-backend | Build+test all 12 backends (push to main only) |
| stale-check | Verify legacy directories are removed |
