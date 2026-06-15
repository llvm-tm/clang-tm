# LLVM IR Instrumentation Analysis: Bank Benchmark

## Overview

This document describes how the TM plugin instruments the Bank benchmark.
The uninstrumented IR can be generated from `benchmarks/plugin/bank/bank.cpp`.
The instrumented IR is produced by `opt -passes="tm-instrument"` via the
`clang-tm` wrapper. See `benchmarks/plugin/bank/Makefile` for the pipeline.

## What Is Annotated

From `llvm.global.annotations` we identify:

| Entity | Annotation | Kind |
|--------|-----------|------|
| `bank` (global variable) | `"tm"` | TM global |
| `transfer(int, int, int)` | `"shared"` | TX function |
| `total_transactional()` | `"shared"` | TX function |
| `reset()` | `"shared"` | TX function |
| `worker_thread(ThreadData&)` | `"thread"` | Thread entry |
| `main` | `"main"` | Main entry |

## What the Plugin Does

### 1. Thread Lifecycle (worker_thread)
Inserts at entry:
```llvm
call void @tm_init_thread()
```
Inserts at all return points:
```llvm
call void @tm_exit_thread()
```

### 2. Main Lifecycle (main)
Inserts at entry:
```llvm
call void @tm_init()
call void @tm_init_thread()
```
Inserts before returns:
```llvm
call void @tm_exit_thread()
call void @tm_exit()
```

### 3. Transaction Boundaries (TX functions)
Each TX function (`transfer`, `total_transactional`, `reset`) gets:
```llvm
call void @tm_begin()
```
at entry, and:
```llvm
call void @tm_end()
```
at each return point.

The function body between `tm_begin()` and `tm_end()` has its loads/stores to
TM-annotated globals replaced with `tm_read_*`/`tm_write_*` calls.

### 4. Memory Access Instrumentation

Loads and stores to addresses that trace back to a TM-annotated global
(here: the `bank` object) are replaced:

- **Load from TM global** → `call i32 @tm_read_i4(ptr %addr)`
- **Store to TM global** → `call void @tm_write_i4(ptr %dest, i32 %val)`

In the Bank benchmark, 9 TM read/write calls are generated across all
TX functions. The `bank` object is a struct with two `int` fields; accesses
to those fields within transaction functions are instrumented.

### 5. Call-Graph Cloning

Functions reachable from TX functions that take TM-traceable arguments
are cloned with the `_tm_clone` suffix. The clone has instrumented
loads/stores; calls within TX context are redirected to the clone.
The original function remains unchanged for non-TM call sites.

## Static Call Graph for Bank

```
worker_thread (THREAD)
  └── transfer (TX)
  │     └── Bank::doTransfer (cloned if accesses TM globals)
  ├── total_transactional (TX)
  ├── reset (TX)
  └── ... non-TX helpers (not cloned)
```

## Why Bank Works While STMbench7 Hangs

| Aspect | Bank | STMbench7 |
|--------|------|-----------|
| TM globals | 1 (small struct) | 17 (large vectors) |
| TX functions | 3 | ~50 |
| TM reads/txn | ~2–64 | up to ~5M |
| Read-set size | tiny | millions |
| Vector iteration | no loops over TM data | range-for over `g_connections` |
| `tracesFromTMGlobal` | never used | traces every iterator access |

The key difference: **read-set (or write-set) size**. STMbench7's long
traversals iterate 300k elements × multiple fields, generating millions
of TM reads per transaction. With `std::unordered_map`, each insertion is
O(1) amortized but hash-table overhead adds up. More critically, `extend()`
(validating the ENTIRE read-set) becomes O(N) per call with N in the
millions. When `version > tx->end_version` triggers `extend()`, the
validation loop iterates all entries — each doing an atomic load + decode.
For a read-set of 5 million entries, this alone could take minutes.

Additionally, the transaction growth phase (first read of each address)
creates millions of `unordered_map` inserts with periodic rehashing,
while the read-phase (`read_set.find`) does hash lookups per access.
With small transactions (Bank), these costs are negligible; with
million-element transactions, they dominate.
