# Transactional Memory Benchmarks

This directory contains various benchmarks for evaluating Software Transactional Memory (STM) implementations.

## Table of Contents

- [STAMP](#stamp)
- [TPC-C](#tpc-c)
- [YCSB](#ycsb)
- [STMbench7](#stmbench7)
- [EigenBench](#eigenbench)
- [Bank](#bank)
- [Data Structures](#data-structures)

---

## STAMP

**Stanford Transactional Applications for Multi-Processing**

STAMP is a benchmark suite designed for transactional memory research. It consists of eight real-world applications adapted for TM systems.

### Benchmarks Included

| Benchmark | Description |
|-----------|-------------|
| **bayes** | Bayesian network structure learning |
| **genome** | Gene sequencing |
| **intruder** | Network intrusion detection |
| **kmeans** | K-means clustering |
| **labyrinth** | Maze routing |
| **ssca2** | Graph kernels |
| **vacation** | Travel reservation system |
| **yada** | Delaunay mesh refinement |

### Specification

> STAMP: Stanford Transactional Applications for Multi-processing
> - Authors: Chi Cao Minh, JaeWoong Chung, Christos Kozyrakis, Kunle Olukotun
> - Published: IISWC 2008

### Official Resources

- **GitHub**: https://github.com/kozyraki/stamp
- **Paper**: https://ieeexplore.ieee.org/document/4636089

---

## TPC-C

**Transaction Processing Performance Council Benchmark C**

TPC-C is an OLTP (Online Transaction Processing) benchmark that simulates a wholesale supplier's order-entry environment. It involves a mix of five concurrent transactions.

### Transactions

| Transaction | Type | Description |
|-------------|------|-------------|
| NewOrder | Read-Write | Enter a complete order (backbone of workload) |
| Payment | Read-Write | Update customer balance and payment |
| OrderStatus | Read-Only | Check order status |
| Delivery | Read-Write | Process batch delivery |
| StockLevel | Read-Only | Check stock level |

### Specification

> TPC-C is an OLTP workload that simulates the activities found in complex OLTP application environments.

### Official Resources

- **Specification**: https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf
- **Website**: https://www.tpc.org/tpcc/

---

## YCSB

**Yahoo! Cloud Serving Benchmark**

YCSB is a framework and common set of workloads for evaluating the performance of different key-value and cloud serving stores.

### Core Workloads

| Workload | Description | Read/Write Ratio |
|----------|-------------|-----------------|
| **A** | Update heavy | 50/50 |
| **B** | Read heavy | 95/5 |
| **C** | Read only | 100/0 |
| **D** | Read latest | 95/5 (inserts) |
| **E** | Short ranges | 95/5 (scans) |
| **F** | Read-modify-write | 50/50 |

### Specification

> Benchmarking Cloud Serving Systems with YCSB
> - Authors: Brian F. Cooper, Adam Silberstein, Erwin Tam, Raghu Ramakrishnan, Russell Sears
> - Published: Yahoo! Research, 2010

### Official Resources

- **GitHub**: https://github.com/brianfrankcooper/YCSB
- **Wiki**: https://github.com/brianfrankcooper/YCSB/wiki

---

## STMbench7

**Software Transactional Memory Benchmark 7**

STMbench7 is a benchmark for evaluating STM implementations. It uses a complex graph-based data structure simulating CAD/CAM applications.

### Characteristics

- Based on assembly trees and indexes
- Contains 45 operations on shared data structures
- Four main operation categories: traversals, updates, reads, structural modifications
- Includes lock-based baselines for comparison

### Specification

> STMBench7: A Benchmark for Software Transactional Memory
> - Authors: Rachid Guerraoui, Michal Kapalka, Jan Vitek
> - Published: EuroSys 2007

### Official Resources

- **Paper**: https://janvitek.org/pubs/eurosys07.pdf
- **ACM**: https://dl.acm.org/doi/10.1145/1272998.1273029

---

## EigenBench

**EigenBench: A Simple Exploration Tool for Orthogonal TM Characteristics**

EigenBench is a synthetic microbenchmark designed to isolate and test various characteristics of TM systems independently.

### Eight Orthogonal Characteristics

| Characteristic | Description |
|----------------|-------------|
| **Concurrency** | Number of concurrent transactions |
| **Transaction Length** | Number of operations per transaction |
| **Working Set Size** | Amount of data accessed |
| **Temporal Locality** | Data reuse patterns |
| **Pollution** | Cache behavior effects |
| **Contention** | Conflict probability |
| **Predominance** | Read vs write ratio |
| **Density** | Spatial locality |

### Specification

> Eigenbench: A simple exploration tool for orthogonal TM characteristics
> - Authors: S. Hong, T. Oguntebi, J. Casper, N. Bronson, C. Kozyrakis, K. Olukotun
> - Published: IISWC 2010

### Official Resources

- **Paper**: https://ieeexplore.ieee.org/document/5648812
- **Stanford**: http://csl.stanford.edu/~christos/software.html

---

## Bank

A simple banking benchmark that tests basic transactional operations (transfers between accounts) to verify correctness (money conservation).

### Specification

This benchmark tests:
- Concurrent transfers between accounts
- Total balance consistency (must remain constant)
- Basic ACID properties

---

## Data Structures

Microbenchmarks based on common data structures:

| Benchmark | Description |
|-----------|-------------|
| **AVL Tree** | Self-balancing binary search tree |
| **Red-Black Tree** | Self-balancing binary search tree |
| **Hash Map** | Key-value store |
| **Bitmap** | Bitmap operations |
| **List** | Linked list operations |
| **Set** | Set operations |
| **Heap** | Priority queue |

---

## Building and Running

Each benchmark directory contains a Makefile with build targets:

```bash
# Build all targets
make all

# Build specific runtime
make bin/benchmark_tl2       # TL2 runtime
make bin/benchmark_tinystm   # TinySTM runtime
make bin/benchmark_swiss     # SwissTM runtime

# Clean build artifacts
make clean
```

## Runtimes

The project supports multiple STM runtimes:

- **TL2** - Transactional Locking 2
- **TinySTM** - Tiny Software Transactional Memory
- **SwissTM** - Swiss Transactional Memory
- **SingleGlobalLock** - Simple global lock baseline (for comparison)

Each runtime is located in `backends/runtimes/` and is selected based on the target name.