# Explicit TM API Benchmarks (C++)

C++ benchmarks using the explicit (expli) TM API — a header-only lambda-based API
that calls the same STM backends as the LLVM plugin, but **without any compiler
plugin**. Compile with any C++20 compiler and link against any backend at build time.

## Table of Contents

- [Quick Start](#quick-start)
- [Selecting a Backend](#selecting-a-backend)
- [Available Backends](#available-backends)
- [Benchmarks](#benchmarks)
- [Running All Benchmarks](#running-all-benchmarks)
- [Correctness Notes](#correctness-notes)
- [Project Structure](#project-structure)

## Quick Start

```bash
cd expli_benchmarks

# Build all benchmarks with the default backend (TinySTM/WBCTL):
make all

# Run a single benchmark:
./bin/bank -d 5000 -a 512 -t 4

# Build and run in one step:
make run-bank       # Bank
make run-fuzz       # fuzz_counter + fuzz_bank
make run-bench7     # STMbench7
make run-vacation   # Vacation (simplified STAMP)
make run-tpcc       # TPC-C
make run-ycsb       # YCSB
make run-eigenbench # EigenBench
make run-tests      # tm_api unit tests (test_ds + test_tx)

# Build a specific backend:
make BACKEND=TL2 all
make BACKEND=NOrec run-bank
make BACKEND=SGL run-fuzz
```

## Selecting a Backend

The backend is selected at **compile time** by setting the `BACKEND` make variable.
The default is `TINYSTM` (which uses TinySTM/WBCTL — write-back commit-time locking).

```bash
# TinySTM/WBCTL (default):
make BACKEND=TINYSTM all

# NOrec (value-based validation):
make BACKEND=NOREC all

# TL2 (commit-time locking):
make BACKEND=TL2 all

# SwissTM (eager locking):
make BACKEND=SWISSTM all

# SingleGlobalLock (serial via mutex, baseline):
make BACKEND=SGL all
```

To change the backend, you must **recompile** the targets:

```bash
make clean && make BACKEND=TL2 run-bank
```

## Available Backends

| `BACKEND=`  | C++ backend          | Strategy                        | Correctness |
|-------------|----------------------|---------------------------------|-------------|
| `TINYSTM` (default) | TinySTM/WBCTL  | Write-back, commit-time locking | All benchmarks pass |
| `NOREC`     | NOrec                | Value-based validation          | Bank/fuzz pass; labyrinth 0 paths |
| `TL2`       | TL2                  | Commit-time locking             | Bank/fuzz pass; labyrinth 0 paths |
| `SWISSTM`   | SwissTM              | Eager-locking + undo log        | Bank passes; labyrinth SIGBUS; stmbench7 bad_alloc |
| `SGL`       | SingleGlobalLock     | `std::mutex` (serial)           | All pass (serial baseline) |

## Benchmarks

All benchmarks accept `-t <threads>` and `-d <duration_ms>`:

```bash
# Bank (money conservation test, default: 512 accounts, 20% read-all, 4 threads, 10s)
./bin/bank -d 3000 -a 1024 -t 4 -r 20

# EigenBench (TM orthogonal characteristics microbenchmark)
./bin/eigenbench -d 3000 -t 4 --r1 10 --w1 10 --a1 100 --a2 10000

# STMbench7 (CAD/CAM graph benchmark, 200 composite parts, 800 atomic parts)
./bin/stmbench7 -d 5000 -t 4

# Vacation (simplified STAMP travel reservation)
./bin/vacation -d 5000 -t 4

# TPC-C (9 tables, 5 transaction types)
./bin/tpcc -t 4 -d 3000 -w 1

# YCSB (workloads A-F, zipfian/uniform/latest distribution)
./bin/ycsb -t 4 -d 3000 -w a

# Fuzz tests (deterministic counter and bank stress tests)
./bin/fuzz_counter -t 4 -n 10000 -c 8 -s 42
./bin/fuzz_bank -t 4 -n 5000 -a 16 -s 42

# expli_tm_api unit tests
./bin/test_ds
./bin/test_tx
```

### Benchmark flags

| Flag        | Applies to  | Meaning                                |
|-------------|-------------|----------------------------------------|
| `-d <ms>`   | All         | Duration in milliseconds               |
| `-t <n>`    | All         | Number of threads                      |
| `-a <n>`    | bank        | Number of accounts                     |
| `-r <pct>`  | bank        | Percentage of read-all transactions    |
| `-w <n>`    | tpcc        | Number of warehouses                   |
| `-w <a..f>` | ycsb        | Workload letter                        |
| `-n <n>`    | fuzz        | Number of iterations per thread        |
| `-c <n>`    | fuzz_counter| Number of counters                     |
| `-s <seed>` | fuzz        | Random seed                            |

## Running All Benchmarks

```bash
cd expli_benchmarks

# Build everything:
make all

# Run all benchmarks with default parameters:
make run              # runs bank, eigenbench, stmbench7, vacation, fuzz, tpcc, ycsb

# Run a specific subset:
make run-bank run-fuzz run-tests
```

## Correctness Notes

Not all backends work correctly for every workload. Here is the current status
(based on tests with TinySTM/WBCTL as the reference):

| Benchmark       | TINYSTM | NOREC | TL2 | SWISSTM | SGL |
|-----------------|:-------:|:-----:|:---:|:-------:|:---:|
| bank            | ✓       | ✓     | ✓   | ✓       | ✓   |
| fuzz_counter    | ✓       | ✓     | ✓   | ✓       | ✓   |
| fuzz_bank       | ✓       | ✓     | ✓   | ✓       | ✓   |
| eigenbench      | ✓       | ✓     | ✓   | ✓       | ✓   |
| stmbench7       | ✓       | ✓     | ✓   | ✗ bad_alloc | ✓ |
| vacation        | ✓       | ✓     | ✓   | ✗ hang   | ✓   |
| labyrinth       | ✓       | ✗ 0 paths | ✗ 0 paths | ✗ SIGBUS | ✓ |
| tpcc            | ✓       | ✓     | ✓   | ✗ hang   | ✓   |
| ycsb            | ✓       | ✓     | ✓   | ✗ hang   | ✓   |

**Key insight**: The crashes in SwissTM are a **backend design limitation** (eager
locking + undo log causes SIGBUS on null addresses from moved-from objects), not
a bug in the expli API. The identical crashes occur when using SwissTM with the
LLVM plugin.

TinySTM/WBCTL (write-back, commit-time locking) passes all workloads and is the
recommended backend for correctness-sensitive applications.

## Project Structure

```
expli_benchmarks/
├── Makefile                          # Build all benchmarks for any backend
├── bank/
│   └── bank.cpp                      # Bank money-conservation benchmark
├── eigenbench/
│   └── eigenbench.cpp                # EigenBench TM microbenchmark
├── STMbench7/
│   └── STMbench7.cpp                 # CAD/CAM graph benchmark
├── vacation/
│   └── vacation.cpp                  # Simplified STAMP vacation
├── tpcc/
│   └── tpcc.cpp                      # TPC-C benchmark
├── ycsb/
│   └── ycsb.cpp                      # YCSB benchmark
├── fuzz/
│   ├── fuzz_counter.cpp              # Deterministic counter stress test
│   └── fuzz_bank.cpp                 # Deterministic bank stress test
└── bin/                              # Compiled binaries
```

The TM API itself lives in `expli_tm_api/tm_api.hpp` and the backends are in
`backends/`. No LLVM plugin, no bitcode pipeline, no cloning — just a C++20
compiler and pthreads.
