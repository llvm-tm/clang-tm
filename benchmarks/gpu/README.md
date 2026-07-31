# GPU TM Benchmarks

Standalone GPU transactional-memory benchmarks. Unlike the plugin-instrumented
benchmarks under `benchmarks/plugin/`, these run **without the LLVM plugin**:
transaction bodies are device (`__device__`) functions that call a GPU TM
backend's API directly (currently CSMV via the batch executor).

## Contents

| File           | Workload                                              |
|----------------|-------------------------------------------------------|
| `gpu_ycsb.cu`  | YCSB-style read/write mix on a shared table, batch-executed |
| `gpu_kmeans.cu`| STAMP kmeans assignment (draft: warp-cooperative distance) |
| `gpu_memcached.cu` | MemcachedGPU-style KV GET/SET (draft)               |
| `gpu_tpcc.cu`  | TPC-C Payment transaction, money-conservation check (draft) |

`gpu_ycsb.cu` is the reference implementation (validated design, see
`REVIEW.md`). The other three are drafts: skeletons demonstrating the
batch-executor + warp-cooperative pattern with simplified algorithms
(TODO markers note simplifications).

## Build

Requires a CUDA or HIP toolchain (no GPU is needed at build time, but one is
needed to run).

```sh
make            # auto-detect nvcc
make CUDA=/opt/cuda/bin/nvcc
make HIP=1      # HIP build (AMD), uses hipcc
make HIP=1 HIPCC=/opt/rocm/bin/hipcc
```

## Run

```sh
make run        # gpu_ycsb 4096 1024 8 50
# or directly:
./bin/gpu_ycsb [records] [transactions] [ops] [write_ratio_percent] [seed]
```

The benchmark checks an invariant: the sum of all committed writes must equal
`commits * writes_per_tx`. It exits non-zero on failure.

## Design notes

- `gpu_ycsb.cu` uses `csmv_gpu_begin/read/write/commit` from the CSMV backend
  (`backends/tm_impl/csmv/gpu/csmv_batch_executor.hpp`). Each warp executes one
  transaction; version-list traversal is warp-cooperative.
- No TM region allocator, no host-side hooks, no `tm_register_real_hooks`:
  the GPU backend is driven directly.
- Final table state is read back with a snapshot kernel that walks version-list
  heads (writes prepend, so the head holds the newest committed value).

## Related work

Literature surveyed while designing these benchmarks. The two reference points
for the batch-executor model are CSMV (in-repo) and Epic; everything else
informs the SIMD/warp-cooperative design constraints documented in `REVIEW.md`.

| System | Reference | Relevance to this directory |
|--------|-----------|------------------------------|
| **Epic** | Qian & Goel, OSDI'24 (USENIX, "Massively Parallel Multi-Versioned Transaction Processing") | Multi-versioned deterministic GPU OLTP. Batches txns into epochs; init phase (Sort→PrefixSumByKey→PostfixSumByKey→GetOpType→GetRWLocation) precomputes read/write version locations so execution is version-search-free; warp-cooperative; epoch-ID sync for RAW, no locks for WAR/WAW. The conceptual target for the static-batching assessment in `docs/gpu_static_txn_batching.md`. |
| **gCCTB** | Sun et al., 2024 (`arxiv.org/abs/2406.10158`) | First systematic GPU CC survey: eight schemes (gputx, gacco, tpl_nw, tpl_wd, to, mvcc, silo, tictoc) evaluated on YCSB + TPC-C on a GPU. Confirms GPU CC schemes are lock-based with abort; deterministic schemes are a newer, separate line. |
| **GPUTx** | He & Yu, 2011 (arXiv:1103.3105) | Origin of the bulk-execution model (group txns into a bulk, one kernel, one stored procedure per txn type). T-dependency-graph + k-set conflict-free batch scheduling — the ancestor of CSMV's batch executor and of the greedy conflict-free packing in `docs/gpu_static_txn_batching.md`. |
| **GaccO** | DFKI (ICDE, "GaccO: A GPU-accelerated OLTP DBMS") | CPU+GPU co-execution; groups same-type txns into vectorized batches; runs DBs larger than device memory via CPU spillover. Name-sake of our `gacco/` backend, though our in-repo GAccO is lock-table GPUTx-style (see `REVIEW.md`/AGENTS note). |
| **LTPG** | Aalborg Univ., 2026 ("Large-Batch Transaction Processing on GPUs with Deterministic Concurrency Control") | Newer deterministic OCC: execution → conflict detection → write-back stages, no dependency-graph maintenance and no predefined read/write sets — an alternative to Epic's version-location precomputation. |
| **MemcachedGPU** | Hetherington et al., SoCC'15 (ACM 2806836) | GPU key-value store on GNoM; request batching, per-set locking, ~13 MRPS at 10GbE line rate. Motivation for `gpu_memcached.cu`. |
| **WarpSpeed / Hive / DACHash** | McCoy & Pandey 2025 (arXiv:2509.16407); Polak et al. 2025 (arXiv:2510.15095); Zhou et al. 2023 | Concurrent GPU hash tables with warp-cooperative protocols (one atomic per warp, shfl/ballot, tiled layouts) and YCSB evaluation. Validates the warp-cooperative + `__shfl`/`__ballot_sync` patterns used by the CSMV batch executor and these benchmarks. |

Key takeaway for this directory: GPU TM workloads are *SIMD-first*. Divergence
(per-thread branching on data) and per-thread atomics are the two main
performance killers, so every benchmark here keeps transaction code warp-uniform
and collapses shared-state mutation to a single lane with `__shfl_sync`/
`__ballot_sync` broadcast (see `REVIEW.md`).

## Adding a backend

To dispatch a GPU backend through the host TM hook API (so instrumented code
can reach the GPU), the backend must expose `tm_begin/tm_read_*/tm_write_*/tm_end`
that transfer data to/from the device. The CSMV batch executor is the reference
model: `enqueue()` batches tx bodies, `launch()` runs them as a kernel, and the
device functions perform TM ops against device-side version lists.
