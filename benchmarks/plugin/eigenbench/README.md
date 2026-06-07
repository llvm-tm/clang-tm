# EigenBench — Synthetic TM Microbenchmark

**Specification**: A Simple Exploration Tool for Orthogonal TM
Characteristics.  Hong, Oguntebi, Casper, Bronson, Kozyrakis,
Olukotun.  IISWC 2010.

## Overview

EigenBench is a synthetic microbenchmark that isolates eight
orthogonal characteristics of TM systems:

| Characteristic      | Description                            |
|---------------------|----------------------------------------|
| **Concurrency**     | Number of concurrent transactions      |
| **Transaction Length** | Operations per transaction           |
| **Working Set Size**| Amount of data accessed                |
| **Temporal Locality**| Data reuse patterns                   |
| **Pollution**       | Cache behaviour effects                |
| **Contention**      | Conflict probability                   |
| **Predominance**    | Read vs. write ratio                   |
| **Density**         | Spatial locality                       |

## Build

```sh
cd benchmarks/plugin/eigenbench

make all                     # Build all backends
make eigen_singlelock        # SingleGlobalLock
make eigen_tl2               # TL2
make eigen_tinystm           # TinySTM
make eigen_uninstrumented    # Baseline
```

## Usage

```sh
./bin/eigen_<backend> -t <threads> -d <duration_ms>
```

## Official Resources

- **Paper**: https://ieeexplore.ieee.org/document/5648812
