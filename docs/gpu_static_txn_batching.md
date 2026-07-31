# Static-Transaction Extraction Layer for GPU Batching (Calvin / GAccO / Epic)

## Problem statement

Calvin, GAccO, and Epic are *deterministic / set-aware* concurrency-control
schemes: their correctness and performance depend on knowing each
transaction's **read set and write set before execution**. On the GPU this
pays off doubly: a batch whose transactions are pairwise conflict-free can
run with zero aborts and no lock traffic, fully utilizing all warps.

Today the repo has no such layer. The LLVM plugin instruments transactions
with `tm_read_*/tm_write_*` calls and executes them *online* (an address is
only known when the instruction runs). GPU backends like PR-STM/CSMV handle
conflicts at runtime via locks/validation. For Calvin/GAccO/Epic we need a
**compile-time + host-side extraction pass** that produces batches.

This document is an assessment: what exists, what we need, the design, and a
step-by-step plan. It does not implement the layer.

## What each backend needs

| Backend | Needs read/write sets a priori? | Conflict handling | Batch granularity |
|---------|-------------------------------|-------------------|-------------------|
| **Calvin** | Yes — deterministic ordering | None (serial order) | All txns, ordered |
| **GAccO** | Yes — lock acquisition plan | Lock-free reads, versioned writes | All txns, conflict-checked |
| **Epic** | Yes — version pre-allocation | None (deterministic) | Epoch (100K txns) |

All three need, at minimum: `(txn_id, address, read/write)` tuples per
transaction, produced *before* the GPU runs anything.

## Existing infrastructure we can reuse

1. **LLVM plugin** (`plugin/passes/TMInstrumentPass.cpp`): already identifies
   TM-annotated globals (`collectTMSymbols`), instruments loads/stores, and
   clones+instruments transaction functions. It can also *enumerate* the
   addresses a transaction touches if we add an analysis pass.
2. **`tm_register_global` / `g_tm_globals`**: already registers static TM
   globals into a vector. Dynamic heap objects are not tracked — this is the
   hard part.
3. **CSMV batch executor** (`csmv_batch_executor.hpp`): host-side `enqueue()`
   + kernel launch. The natural scheduling target for extracted batches.
4. **Queue runtime** (`queue_runtime.cpp`): worker threads + `tm_enqueue` —
   an alternative "execute on host, batch on GPU" path.
5. **The hooks system** (`tm_hooks.hpp`): all 21 ops go through function
   pointers, so we can install a *tracing* hook set that records accesses
   instead of performing them.

## Two extraction strategies

### Strategy A: Runtime address tracing (reconnaissance / dry run)

Run each transaction body once on the host against a shadow or real copy,
with tracing hooks that log every `(addr, type)` access. This is exactly
Epic's *read-write set identification* ("reconnaissance") and Calvin's
*declarative request* analysis — but instead of SQL, our "requests" come
from running the code.

- **How**: install a `TMRealHooks` set whose `read_i*/write_i*` append
  `(addr, type, txn_id)` to a per-txn log and return the shadow value
  (or read the real value — side effects are the problem).
- **Problem — side effects**: `tm_malloc`/`tm_calloc` return real pointers;
  a second run returns different pointers, so addresses are not stable.
  `tm_free` cannot be called twice. Need **deterministic allocation**: a
  shadow allocator with a fixed seed/offset so the same object gets the same
  address on every run.
- **Problem — data dependence**: reads return values that drive branches
  (`if (node->right) ...`). A dry run must see *committed* values or the
  traced address set is wrong. For benchmarks whose tx bodies are
  data-independent loops (fuzz_counter, bank, YCSB, TPC-C OLTP), the address
  set is stable; for graph walks (rbtree, avltree) it is not.
- **Verdict**: correct for the OLTP workloads we target (the gpu_cc_tb /
  Epic targets), fragile for pointer-chasing workloads. Cheap to build
  (~300 lines) using the existing hook dispatch.

### Strategy B: Compile-time static enumeration (LLVM analysis pass)

A new pass that, for each transaction function, computes the *set of
possibly-touched addresses* statically:
- For `tm_read_i4(&g_counter)` → the address of `g_counter` is a constant.
- For `tm_read_i4(&g_table[i])` where `i` comes from a bounded loop or a
  known key → use range analysis / symbolic execution to bound the set.
- For dynamic `&node->right` from a pointer returned by `tm_malloc` → only
  sound if we can prove an allocation-site relationship. Generally **not
  statically decidable** without running.

- **Verdict**: works for direct global references; fails (conservatively
  over-approximates to "all addresses") for heap-linked structures. Use as a
  *fast path* for globals, fall back to Strategy A for heap.

**Recommendation: Strategy A as the primary mechanism, Strategy B as an
optimization for static globals.** This matches how Epic actually works (it
runs reconnoitered reads on the GPU to get exact sets).

## The extraction layer: design

### Component 1: `TracingHooks` (host-side)

Install at `tm_init` (or via `tm_swap_runtime`):

```
struct AccessLogEntry {
    uint64_t    addr;
    uint8_t     rw;      // 0=read, 1=write
    uint32_t    txn_id;
    uint32_t    op_id;   // order within the transaction
};
```

- `real_tm_begin()` sets a global "collecting" flag; subsequent
  `read_*/write_*` append to a thread-local or global `AccessLog`.
- `real_tm_end()` finalizes the current txn's log into a `TxnAccessSet`.
- For determinism, hook `real_tm_malloc/calloc` into a shadow allocator that
  maps `(object_id, size)` → a fixed address in a scratch arena
  (`0x7f01_0000_0000 + id * 4096`), guaranteeing stable addresses across
  dry runs.
- `tm_free` is a no-op during collection.

### Component 2: batch scheduler

Input: `TxnAccessSet[]` (one per txn). Output: an ordered list of batches
`B_0, B_1, ...`.

```
for each txn T in input order:
    conflicts = any addr in T.readset/writeset that is in
                any committed txn's write-set in the current batch
    if conflicts:  close batch, start new one with T
    else:          add T to current batch
```

Conflict predicate (same as Epic/GAccO batch partitioning):

```
Conflicts(A, B) := (A.writes ∩ (B.reads ∪ B.writes)) ≠ ∅
```

This is the classic **interval-graph coloring / batch packing** problem.
Greedy first-fit is O(n) per txn with a hash set and near-optimal for
OLTP workloads; can be parallelized on host. Since determinism is not
required for GAccO, we can also reorder txns to pack better (Epic keeps
transaction order fixed for deterministic serialization; GAccO does not
care). **Configurable ordering policy.**

### Component 3: GPU dispatch

The resulting batches feed the CSMV batch executor (or a new Calvin/Epic
kernel):

- **Epic-style execution**: kernel receives `(record_ids[], op_ids[],
  rw_types[])` per txn; the init phase has already precomputed version
  locations, so execution does direct table reads/writes with eid-based
  publish. No validation, no locks.
- **GAccO-style execution**: txns in a conflict-free batch run with
  lock-free reads and versioned writes; the precomputed sets mean no
  runtime lock conflicts within a batch.
- **Calvin-style execution**: txns run in deterministic serial order; the
  scheduler has already ordered them, execution just replays.

### Component 4: integration points

- Hook into `csmv_batch_executor` (add a `enqueue_from_access_sets()` path
  that skips the function-pointer body and instead feeds the precomputed
  op lists).
- Expose as a host library `libstatic_batch` that any benchmark links
  against, or as a second LLVM pass that rewrites the transaction into a
  "batch executor" form.
- The extraction runs **once per input data set** (like Epic's indexing),
  not per epoch: address sets only change when the data/keys change.

## Effort estimate

| Component | Lines | Risk |
|-----------|-------|------|
| TracingHooks + shadow allocator | ~300 | Low (hooks exist) |
| Batch scheduler (greedy packing) | ~200 | Low |
| Epic-style execution kernel | ~400 | Medium |
| GAccO integration | ~300 | Medium |
| Calvin integration | ~200 | Low |
| Benchmark harness (fuzz/bank/YCSB → batch) | ~200 | Low |
| **Total** | **~1600** | |

## Verification

1. **Correctness**: run extracted batches against a reference backend
   (e.g., TinySTM) on the same input; compare final state + per-txn
   commit/abort decisions. All tests in `tests/` should agree.
2. **Invariant for batch packing**: verify no two txns in one batch
   conflict on a write address (re-check at runtime in debug mode).
3. **Throughput**: compare batch execution vs online execution on
   fuzz_counter, bank, and YCSB with increasing contention (theta).
4. **TLA+**: the batch scheduler's greedy invariant ("no write-set
   overlap within a batch") is a simple model — reuse `Calvin.tla` /
   `GPU_CSMV.tla` structure.
