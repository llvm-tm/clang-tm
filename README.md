# TM API — Transactional Memory Framework

Multi-paradigm transactional memory framework for C++ (LLVM plugin, explicit C++ API, Rust bindings).
Supports 12+ STM/HTM backends.

## Structure

```
backends/          — TM runtime implementations (TinySTM, NOrec, TL2, SGL, SwissTM, etc.)
plugin/            — LLVM instrumentation plugin (5-pass Honorio pipeline)
benchmarks/        — Benchmarks (plugin-instrumented, C++ explicit, Rust)
tests/             — Tests for all components
expli_instr/       — Explicit C++ API headers + Rust workspace
tools/             — Build/install scripts
simulator/         — Rust discrete event simulator for TM trace replay
```

## Quick Start — Explicit C++ API (no LLVM plugin needed)

```bash
# Build and run with TinySTM (default)
make -C benchmarks/cpp -j4 run-test-tx

# Select a different backend
make -C benchmarks/cpp -j4 BACKEND=NOREC bin/test_tx
./benchmarks/cpp/bin/test_tx

# Run a benchmark
make -C benchmarks/cpp -j4 BACKEND=TINYSTM bin/bank
./benchmarks/cpp/bin/bank -t 2 -d 1000 --test
```

Available backends: `TINYSTM`, `WBETL`, `WT`, `NOREC`, `SWISSTM`, `TL2`, `SGL`, `LEFTRIGHT`, `ROMULUS`, `XTM`, `SPHT`, `TSXSGL`.

## Quick Start — LLVM Plugin

```bash
# 1. Build the plugin
make plugin

# 2. Build a plugin-instrumented benchmark
cd benchmarks/plugin/bank
make bank_singlelock
./bin/bank_singlelock -t 4 -d 5000
```

## Quick Start — Rust API

```bash
cd benchmarks/rust

# Build and run bank with NOrec
cargo run --release --no-default-features --features tm/norec --bin bank -- -d 100 -t 2 --test

# With default TinySTM backend
cargo run --release --bin bank -- -d 100 -t 2 --test
```

## Build and Run All Tests

```bash
# Explicit C++ API across all 12 backends
make check-all

# Or use the smoke test
./smoke_test.sh
```

## All Backend-Specific Plugin Benchmarks

| Backend       | Define                | Notes                        |
|---------------|-----------------------|------------------------------|
| TinySTM/WBCTL | `DESIGN_WBCTL`        | Write-back commit-time lock  |
| TinySTM/WBETL | `DESIGN_WBETL`        | Write-back encounter-time    |
| TinySTM/WT    | `DESIGN_WT`           | Write-through + undo log     |
| NOrec         | —                     | Lazy value-based validation  |
| TL2           | —                     | Commit-time locking          |
| SwissTM       | —                     | Hybrid lazy/pessimistic      |
| SingleLock    | —                     | Serial execution             |
| LeftRight     | —                     | Concurrent read, serialized  |
| Romulus       | —                     | Redo logging                 |
| XTM           | —                     | Experimental                 |
| SPHT          | `-mrtm`               | RTM + epoch commit log       |
| TSXSGL        | `-mrtm`               | TSX + single global lock     |
| DUDETM        | `DESIGN_WBCTL`        | Commit + redo log (plugin)   |
| NVHTM         | `-mrtm`               | RTM + NVM (plugin)           |

## Plugin Race Checker

```sh
opt-22 -load-pass-plugin=plugin/bin/libTMRaceChecker.so \
       -passes="tm-race-checker" myapp.bc -o /dev/null
```

## Known Issues

| Issue | Details |
|-------|---------|
| **LeftRight test_tx fails** | 29/114 tests fail (pre-existing algorithm bug, not related to build fixes) |
| **Romulus test_tx fails** | 54/114 tests fail (pre-existing algorithm bug, not related to build fixes) |
| **DUDETM, NVHTM, DistributedSGL, PersistentSGL** | Build but depend on plugin-provided symbols (`tm_symbol_count`, `tm_symbol_addresses`, `tm_symbol_sizes`) not available in explicit C++ API |
| **rbtree benchmark timing** | Reports 0ms duration (pre-existing) |
| **stmbench7 times out** | Data race in `ts_multimap::lower_bound()` (pre-existing) |
| **Plugin pipeline tests** | Need `clang-tm` wrapper testing (auto-link of `tm_hooks.cpp` fix not fully verified) |
