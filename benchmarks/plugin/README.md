# Transactional Memory Benchmarks

This directory contains benchmarks for evaluating Software
Transactional Memory (STM) implementations using the LLVM TM plugin.

## Table of Contents

- [Building](#building)
- [Available Benchmarks](#available-benchmarks)
- [Quick Start](#quick-start)
- [Benchmark Parameters](#benchmark-parameters)

## Building

Each benchmark directory has a Makefile that builds all backends:

```sh
cd benchmarks/plugin/bank

# Build all backends
make all

# Build specific backend
make bank_singlelock
make bank_tl2
make bank_norec
make bank_tinystm
make bank_swiss

# Build uninstrumented baseline (no TM calls at all)
make bank_uninstrumented
```

The build uses the `tm_pipeline.mk` shared include
(`plugin/tm_pipeline.mk`), which automates the 4-step pipeline:

1. `clang++ -emit-llvm` → LLVM bitcode (`.bc`)
2. `opt -load-pass-plugin=libTMInstrument.so` → instrumented bitcode (`.instr.bc`)
3. `opt -O3` → optimised bitcode (`.opt.bc`)
4. `clang++` + runtime file → executable (under `bin/`)

### Selecting a pipeline variant

Default pipeline is `tm-instrument-inline` (inlines all clones then instruments).
Override via `TM_INSTRUMENT_PIPELINE`:

```sh
# Non-inline pipeline (debug-friendly, clones survive as separate functions)
make TM_INSTRUMENT_PIPELINE=tm-instrument bank_singlelock

# Or BUILD_TYPE=DEBUG (also sets -O0 post-opt, debug link flags)
BUILD_TYPE=DEBUG make bank_singlelock

# Experimental: instrument clones individually before inlining
make TM_INSTRUMENT_PIPELINE=tm-instrument-then-inline bank_singlelock
```

### Using a different plugin variant

```sh
make TM_PLUGIN=../../plugin/bin/libTMInstrument_no_setjmp.so bank_singlelock
```

### Cross-benchmark builds

From the project root, you can run any benchmark:

```sh
make -C benchmarks/plugin/bank bank_singlelock
make -C benchmarks/plugin/datastructures hashmap_SingleGlobalLock
make -C benchmarks/plugin/STAMP stamp_tl2
make -C benchmarks/plugin/stmbench7 stmbench_tinystm
make -C benchmarks/plugin/tpcc tpcc_persistentsgl
make -C benchmarks/plugin/ycsb ycsb_singlelock
make -C benchmarks/plugin/eigenbench eigen_norec
```

## Available Benchmarks

| Benchmark     | Directory          | Description                                                    | README                                |
|---------------|--------------------|----------------------------------------------------------------|---------------------------------------|
| **Bank**      | `test/bank/`       | Money-transfer correctness test (ACID conservation)            | [README](test/bank/README.md)         |
| **Data Structures** | `datastructures/` | Microbenchmarks for AVL tree, RB tree, hash map, bitmap, etc. | [README](datastructures/README.md)    |
| **STAMP**     | `STAMP/`           | 8 real-world TM workloads (vacation, bayes, genome, etc.)      | [README](STAMP/README.md)             |
| **STMbench7** | `STMbench7/`       | CAD/CAM graph-based STM benchmark                              | [README](STMbench7/README.md)         |
| **TPC-C**     | `TPCC/`            | Order-entry OLTP benchmark                                     | [README](TPCC/README.md)              |
| **YCSB**      | `YCSB/`            | Cloud-serving key-value workloads                              | [README](YCSB/README.md)              |
| **EigenBench**| `EigenBench/`      | Synthetic TM microbenchmark for orthogonal characteristics     | [README](EigenBench/README.md)        |

## Quick Start

```sh
# Verify the toolchain works
cd benchmarks/plugin/bank
make bank_singlelock
./bin/bank_singlelock -t 2 -a 256 -d 3000 -r 10 -w 0

# Run all benchmarks with SingleGlobalLock
cd benchmarks
for d in plugin/bank plugin/datastructures plugin/STAMP plugin/stmbench7 plugin/tpcc plugin/ycsb plugin/eigenbench; do
    make -C "$d" all
done
```

## Benchmark Parameters

Most benchmarks accept these common flags:

| Flag   | Default  | Description                         |
|--------|----------|-------------------------------------|
| `-t`   | 2        | Number of threads                   |
| `-d`   | 10000    | Duration in milliseconds            |
| `-r`   | 10       | Read-only transactions (%)          |
| `-w`   | 0        | Write-only transactions (%)         |

See each benchmark's README for benchmark-specific parameters
and command-line options.
