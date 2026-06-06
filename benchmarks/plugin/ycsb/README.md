# YCSB — Yahoo! Cloud Serving Benchmark

**Specification**: Benchmarking Cloud Serving Systems with YCSB.
Cooper, Silberstein, Tam, Ramakrishnan, Sears.  Yahoo! Research, 2010.

## Overview

YCSB is a key-value store benchmark with six core workloads:

| Workload | Description              | Read/Write/Modify |
|----------|--------------------------|--------------------|
| **A**    | Update heavy             | 50/50             |
| **B**    | Read heavy               | 95/5              |
| **C**    | Read only                | 100/0             |
| **D**    | Read latest              | 95/5 (+ inserts)  |
| **E**    | Short ranges             | 95/5 (scans)      |
| **F**    | Read-modify-write        | 50/50 (RMW)       |

## Build

```sh
cd benchmarks/plugin/ycsb

make all                     # Build all backends
make ycsb_singlelock         # SingleGlobalLock
make ycsb_tl2                # TL2
make ycsb_tinystm            # TinySTM
make ycsb_uninstrumented     # Baseline
```

## Usage

```sh
./bin/ycsb_<backend> -t <threads> -d <duration_ms> -w <workload>
```

Workload selection: `-w A` through `-w F`.

## Official Resources

- **GitHub**: https://github.com/brianfrankcooper/YCSB
- **Wiki**: https://github.com/brianfrankcooper/YCSB/wiki
