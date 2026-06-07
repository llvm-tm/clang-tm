# STMbench7 — STM Benchmark 7

**Specification**: STMBench7: A Benchmark for Software Transactional
Memory.  Guerraoui, Kapalka, Vitek.  EuroSys 2007.

## Overview

STMbench7 uses a complex graph-based data structure (composite
pattern with assembly trees, indexed bags, and connections) to
simulate CAD/CAM applications.  It includes 45 operations across
4 categories:

| Category            | Description                                    |
|---------------------|------------------------------------------------|
| **Traversal**       | Walk the assembly tree, read vertex data        |
| **Update**          | Modify vertex data within a sub-graph           |
| **Read**            | Read operations on indexes and vertices         |
| **Structural**      | Add/remove graph elements                       |

## Build

```sh
cd benchmarks/plugin/stmbench7

# Build all variants
make all

# Specific backends
make stmbench_singlelock
make stmbench_tl2
make stmbench_tinystm

# Uninstrumented baseline
make stmbench_uninstrumented
```

## Usage

```sh
./bin/stmbench_<backend> -t <threads> -d <duration_ms>
```

Standard parameters (from the paper):

| Parameter | Default | Description                        |
|-----------|---------|------------------------------------|
| `-t`      | 4       | Number of threads                  |
| `-d`      | 10000   | Duration in milliseconds           |

## Official Resources

- **Paper**: https://janvitek.org/pubs/eurosys07.pdf
