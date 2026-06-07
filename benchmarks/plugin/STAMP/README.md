# STAMP Benchmark Suite - Modern C++ Implementation

Based on: STAMP: Stanford Transactional Applications for Multi-Processing  
Authors: Chi Cao Minh, JaeWoong Chung, Christos Kozyrakis, Kunle Olukotun  
Published: IISWC 2008

GitHub: https://github.com/kozyraki/stamp

## Overview

STAMP is a benchmark suite designed for transactional memory research. It consists of 8 real-world applications adapted for TM systems.

## Benchmarks

| Benchmark | Description | Characteristics |
|-----------|-------------|-----------------|
| **bayes** | Bayesian network structure learning | Medium read, medium write |
| **genome** | Gene sequencing | Low contention, long transactions |
| **intruder** | Network intrusion detection | High contention |
| **kmeans** | K-means clustering | Data parallel, regular access |
| **labyrinth** | Maze routing | Complex, irregular access |
| **ssca2** | Graph kernels | Graph traversal |
| **vacation** | Travel reservation system | High contention, mixed workload |
| **yada** | Delaunay mesh refinement | Complex, iterative |

## Usage

```bash
# Build all versions
make all

# Build specific version
make stamp_uninstrumented  # Native (no TM)
make stamp_tl2              # TL2 runtime
make stamp_tinystm          # TinySTM runtime
make stamp_swiss            # SwissTM runtime

# Run benchmark
./bin/stamp_uninstrumented -t 4 -d 10000 -b bayes
```

## Options

- `-t <n>` - Number of threads (default: 4)
- `-d <ms>` - Duration in milliseconds (default: 10000)
- `-b <bench>` - Benchmark to run (default: bayes)
  - `b` - bayes
  - `g` - genome
  - `i` - intruder
  - `k` - kmeans
  - `l` - labyrinth
  - `s` - ssca2
  - `v` - vacation
  - `y` - yada

## Implementation Notes

This is a modern C++ reimplementation that uses:
- C++17 standard library
- LLVM TM plugin annotations (`TM` and `TX` attributes)
- Header-only data structures where possible
- Simplified but representative implementations of each benchmark

The actual STAMP benchmarks (in the parent `STAMP/` directory) are complex C programs with many files. This version provides a simplified, single-file implementation that captures the essence of each benchmark while being easier to compile and understand.

## Build Targets

| Target | Description |
|--------|-------------|
| `stamp_uninstrumented` | Native execution without TM |
| `stamp_tl2` | Executable with TL2 runtime |
| `stamp_tinystm` | Executable with TinySTM runtime |
| `stamp_swiss` | Executable with SwissTM runtime |