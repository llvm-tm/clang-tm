# TM API — C++ Transactional Memory Framework

Multi-paradigm transactional memory framework for C++ (LLVM plugin, explicit C++ API, Rust bindings).

## Structure

```
backends/          — TM runtime implementations
├── stubs/         — TM-lite compatible pass-through stubs
└── tm_impl/       — 15+ STM/HTM backends (TinySTM, NOrec, TL2, SGL, etc.)
plugin/            — LLVM instrumentation plugin (5-pass Honorio pipeline)
benchmarks/        — Benchmarks (plugin-instrumented, C++ expli, Rust)
tests/             — Tests for all components
tools/             — Build/install scripts
docs/              — Documentation
```

## Quick Start

```bash
# 1. Build the LLVM instrumentation plugin
make plugin

# 2. Build a benchmark and run it
cd benchmarks/plugin/bank

# Single global lock (simplest backend, no TM needed)
make bank_singlelock
./bin/bank_singlelock -t 4 -d 5000

# With a TM backend (e.g. TinySTM write-back commit-lock)
make bank_tinystm
./bin/bank_tinystm -t 4 -d 5000
```

Select a different backend: `export BACKEND=tl2` (default: `tinystm`).

## Building Benchmarks

All build commands assume `cd benchmarks/plugin/<name>` first.

### Bank (transfer between accounts)

```bash
cd benchmarks/plugin/bank
make bank_singlelock              # single global lock
make bank_tinystm                 # TinySTM wbctl
make bank_tinystm_wbetl           # TinySTM encounter-time lock
make bank_tinystm_wt              # TinySTM write-through
make bank_norec                   # NOrec
make bank_tl2                     # TL2
make bank_swiss                   # SwissTM

# Run: flags: -t N (threads, default 4), -d N (duration ms, default 10000)
./bin/bank_tinystm -t 4 -d 5000
```

### STMbench7 (OO7-style benchmark, EuroSys 2007)

```bash
cd benchmarks/plugin/stmbench7
make stmbench_singlelock          # single global lock (recommended)
make stmbench_norec               # NOrec
make stmbench_tl2                 # TL2
# Run:
./bin/stmbench_singlelock
```

### STAMP (Stanford TM benchmark suite)

```bash
cd benchmarks/plugin/STAMP
make stamp_singlelock
./bin/stamp_singlelock -b kmeans -t 4
```

### Data Structures (AVL tree, hashmap, etc.)

```bash
cd benchmarks/plugin/datastructures
make avltree_SingleGlobalLock
./bin/avltree_SingleGlobalLock 1 1000 1000
```

## Backend Unit Tests

Build and run the 36 backend tests (6 tests × 6 backends):

```bash
make -C tests/backends/tm_impl all
make -C tests/backends/tm_impl run
```

## Key Features

- **5-pass decomposed pipeline** (Honorio-style): DualPathInfoCollector → TransactionSafeCreation → ReplaceCallInsideTransaction → LoadStoreBarrierInsertion → Cleanup
- **TM-lite pre-processing pass**: lowers `atomic do` blocks to the same pipeline
- **Multi-backend**: pluggable STM backends via extern "C" ABI
- **Multi-paradigm**: LLVM plugin, explicit C++ API, Rust bindings
- **Annotation-driven barrier elision**: `tm_local` qualifier reduces instrumentation overhead

## Bank Benchmark Options

| Flag | Description | Default |
|------|-------------|---------|
| `-t N` | Number of threads | 4 |
| `-d N` | Duration (milliseconds) | 10000 |
| `-a N` | Number of accounts | 1024 |
| `-r N` | Read-all percentage | 20 |
| `-w N` | Write-all percentage | 0 |
| `-j`  | Disjoint access mode | off |
