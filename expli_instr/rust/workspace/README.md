# Rust TM API

A pure-Rust implementation of the same STM backends (TinySTM/WBCTL, WBETL, WT,
NOrec, TL2, SwissTM, DUDETM, TSXSGL, NVHTM, SPHT) used by the C++ LLVM plugin,
with an explicit lambda-based API. No compiler plugin needed — just `cargo build`.

## Table of Contents

- [Quick Start](#quick-start)
- [How Backend Selection Works (`--no-default-features`)](#how-backend-selection-works---no-default-features)
- [Available Backends](#available-backends)
- [Benchmarks](#benchmarks)
- [Running Tests](#running-tests)
- [Project Structure](#project-structure)

## Quick Start

```bash
# Default backend: TinySTM/WBCTL
cargo run --release --bin fuzz_counter -- -t 2 -n 10000 -c 8

# With a different backend (NOrec):
cargo run --release --no-default-features --features norec --bin fuzz_counter -- -t 2 -n 10000 -c 8

# List all available benchmark binaries:
cargo run --release --bin bank -- --help
cargo run --release --bin eigenbench -- --help
cargo run --release --bin stmbench7 -- --help
cargo run --release --bin stamp -- --help
cargo run --release --bin tpcc -- --help
cargo run --release --bin ycsb -- --help
cargo run --release --bin datastructures -- --help
cargo run --release --bin fuzz_counter -- --help
cargo run --release --bin fuzz_bank -- --help
```

## How Backend Selection Works (`--no-default-features`)

The `tm` crate uses Cargo **feature flags** to select which STM backend to
compile. Exactly **one** backend feature must be active.

The default feature is `wbctl` (TinySTM write-back commit-time locking).
To use a different backend, you must:

1. **Disable the default feature** with `--no-default-features`
2. **Enable the exact one backend** you want with `--features <name>`

```bash
# Wrong: enables BOTH wbctl (default) AND tl2
cargo run --features tl2 --bin bank
# Error: duplicate re-exports of tm_read_u8, tm_write_u8, etc.

# Correct: only tl2 is enabled
cargo run --release --no-default-features --features tl2 --bin bank

# Correct: only norec is enabled
cargo run --release --no-default-features --features norec --bin bank
```

### Why?
Cargo does not track feature exclusivity at the semantic level. If two features
both re-export `tm_read_u8`, you get duplicate symbol errors. The `tm` crate
uses `#[cfg]` guards to route to exactly one backend, but you must ensure only
one feature flag is set.

### Convenience: default features
For day-to-day use with the default (wbctl) backend, just omit the flags:

```bash
cargo run --release --bin bank
```

is equivalent to:

```bash
cargo run --release --features wbctl --bin bank
```

## Available Backends

| Feature flag   | Backend            | Strategy                        |
|----------------|--------------------|---------------------------------|
| `wbctl` (default) | TinySTM/WBCTL   | Write-back, commit-time locking |
| `wbetl`        | TinySTM/WBETL      | Encounter-time locking          |
| `wt`           | TinySTM/WT         | Write-through + undo log        |
| `norec`        | NOrec              | Value-based validation          |
| `tl2`          | TL2                | Commit-time locking             |
| `swisstm`      | SwissTM            | Eager-locking + undo log        |
| `dudetm`       | DUDETM             | Commit-time + redo log          |
| `tsxsgl`       | TSXSGL             | Single global lock (serial)     |
| `nvhtm`        | NVHTM              | RTM + NVM persistence           |
| `spht`         | SPHT               | RTM + epoch-based commit log    |

### Correctness note
Not all backends handle every workload correctly:

- **WBCTL** passes all 9 benchmarks (bank, stmbench7, datastructures,
  eigenbench, stamp, tpcc, ycsb, fuzz_counter, fuzz_bank).
- **WT** and **WBETL** pass most, but may timeout on labyrinth (STAMP).
- **NOrec**, **TL2** pass bank + fuzz + stmbench7, but give 0 routed paths on labyrinth.
- **SwissTM** passes bank + counter, but **hangs/crashes** on complex workloads
  (labyrinth SIGBUS, stmbench7 bad_alloc, fuzz_counter timeout under contention).
  This is a design-level limitation of SwissTM's eager-locking protocol,
  not a Rust-specific bug — the identical issue occurs in C++.

## Benchmarks

All benchmarks accept `-t <threads>` and `-d <duration_ms>`:

```bash
# Bank (money conservation test)
cargo run --release --bin bank -- -t 4 -d 5000 -a 1024 -r 20

# EigenBench (TM characteristics microbenchmark)
cargo run --release --bin eigenbench -- -t 4 -d 5000 --r1 10 --w1 10

# STMbench7 (CAD/CAM benchmark)
cargo run --release --bin stmbench7 -- -t 4 -d 5000

# STAMP (vacation / labyrinth / kmeans)
cargo run --release --bin stamp -- -t 4 -d 5000 -b vacation
cargo run --release --bin stamp -- -t 4 -d 5000 -b labyrinth

# TPC-C (9 tables, 5 transaction types)
cargo run --release --bin tpcc -- -t 4 -d 5000 -w 1

# YCSB (workloads A-F)
cargo run --release --bin ycsb -- -t 4 -d 5000 -w a

# Fuzz tests (deterministic stress tests)
cargo run --release --no-default-features --features wbctl --bin fuzz_counter -- -t 4 -n 10000 -c 8 -s 42
cargo run --release --no-default-features --features wbctl --bin fuzz_bank -- -t 4 -n 5000 -a 16 -s 42
```

## Running Tests

```bash
# All benchmark defaults:
cargo run --release --bin bank
cargo run --release --no-default-features --features tl2 --bin bank

# Verify correctness across all fuzz benchmarks:
for feat in wbctl wbetl wt norec tl2 swisstm; do
  echo "=== $feat ==="
  cargo run --release --no-default-features --features $feat --bin fuzz_counter -- -t 4 -n 5000 -c 8
  cargo run --release --no-default-features --features $feat --bin fuzz_bank -- -t 4 -n 2000 -a 16
done
```

## Project Structure

```
expli_instr/rust/workspace/
├── Cargo.toml                # Workspace root
├── runtime/
│   ├── core/                 # Shared types: Primitive, TypedValue, WriteBack, TmRaw
│   ├── tinystm/              # TinySTM backends (wbctl, wbetl, wt)
│   ├── norec/                # NOrec backend
│   ├── tl2/                  # TL2 backend
│   ├── swisstm/              # SwissTM backend
│   ├── dudetm/               # DUDETM backend
│   ├── tsxsgl/               # TSXSGL backend
│   ├── nvhtm/                # NVHTM backend
│   └── spht/                 # SPHT backend
├── tm/                       # Public API crate (TmCell, Transaction, transaction!)
└── benchmarks/               # Benchmark binaries
    ├── bank.rs
    ├── eigenbench.rs
    ├── stmbench7.rs
    ├── stamp.rs
    ├── tpcc.rs
    ├── ycsb.rs
    ├── datastructures.rs
    ├── fuzz_counter.rs
    └── fuzz_bank.rs
```
