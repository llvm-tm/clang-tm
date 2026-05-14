# Data Structure Benchmarks

Microbenchmarks for common concurrent data structures using TM.

## Benchmarks

| Benchmark | Description | Operations |
|---|---|---|
| **AVL Tree** | Self-balancing BST | insert, erase, contains, rangeCount |
| **Red-Black Tree** | Self-balancing BST | insert, erase, contains |
| **Hash Map** | Open-addressing hash table | insert, erase, contains, get |
| **Linked List** | Sorted linked list | insert, erase, contains |
| **Set** | Ordered set | insert, erase, contains |
| **Bitmap** | Atomic bitmap | set, clear, test |
| **Heap** | Binary heap | push, pop, top |

## Usage

```bash
./bin/<name>_<backend> [threads] [initial_size] [duration_ms] [read%] [write%] [range_max]
```

All arguments are positional:

```bash
# AVL tree, 4 threads, 10000 initial size, 5s, 80% reads, 10% writes
./bin/avltree_SingleGlobalLock 4 10000 5000 80 10 1000
```

## Build

```bash
# Single backend variant
make avltree_NOrec
make hashmap_SingleGlobalLock
make rbtree_TinySTM

# All variants for all data structures
make all
```

## NOrec Results (4 threads, 5s, 80/10/10)

| Benchmark | Throughput |
|---|---|
| Hash Map | 1.3M ops/sec |
| Set | 6k ops/sec |
| AVL Tree | ✅ (correct, slower) |
