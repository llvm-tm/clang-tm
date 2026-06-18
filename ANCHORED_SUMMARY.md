# TM API — Anchored Summary

## Goal
A multi-paradigm C++ transactional memory framework (LLVM plugin, explicit C++ API, Rust bindings) with three instrumentation pipelines, supporting 15+ STM/HTM backends, targeting a paper submission.

## Constraints & Preferences
- **LLVM 22+** plugin using the 5-pass Honorio decomposition (Clone → Collect → Redirect → InstrumentFn → Cleanup)
- Three pipeline variants: `tm-instrument` (standard), `tm-instrument-inline` (inline-then-instrument), `tm-instrument-queue` (queue-based dispatch)
- Backends: TinySTM (wbctl/wbetl/wt), TL2, NOrec, SwissTM, SingleLock, TSXSGL, PersistentSGL, DistributedSGL, DUDETM, SPHT
- Benchmarks: bank, rbtree, STAMP (7 benchmarks), STMbench7, EigenBench, TPC-C, YCSB

## Progress

### Done
- ✓ **5-pass Honorio pipeline** — TMInstrumentPass decomposed into ClonePass, CollectPass, RedirectPass, InstrumentFnPass, CleanupPass with `tm_allow_opaque` semantics
- ✓ **Three pipeline variants all working** — `tm-instrument`, `tm-instrument-inline`, `tm-instrument-queue` produce correct binaries
- ✓ **rbtree benchmark bugs fixed** — `sentinel = -2` out-of-bounds UB changed to `sentinel = MAX_NODES - 1` with proper sentinel node initialization; `insert()` duplicate check added (key comparison + update-on-duplicate, was allocating new nodes until array overflow); bounds check in `newNode()` aborts cleanly on pool exhaustion
- ✓ **rbtree benchmarked on all 3 pipelines** — All pass correctness. Performance within 1% of each other because ~80% of operations use non-void `txn_contains()` which runs inline regardless of pipeline
- ✓ **bank benchmarked on all 3 pipelines** — Queue pipeline is 27–37% faster than standard, with 62% fewer aborts. Inline and standard are neck-and-neck
- ✓ **Queue pipeline architecture documented** — `tm_wait_prev_tx()` injection location, `async_transaction` annotation for batch processing, pending counter mechanism, backend integration details
- ✓ All 8 STAMP benchmarks ported to C++ explicit API and Rust
- ✓ 15+ STM/HTM backends with correctness verification
- ✓ Comprehensive benchmark runner with automated CSV results and plots

### In Progress
- Paper writing and results collection
- Performance analysis across pipeline variants
- Improvement plan execution (see IMPROVEMENT_PLAN.md)

### Blocked
- None

## Key Decisions
1. **5-pass decomposition** — Split monolithic TMInstrumentPass into Clone → Collect → Redirect → InstrumentFn → Cleanup for modularity and correctness
2. **Pipeline default is `tm-instrument`** (non-inline) — avoids write-set/memory asymmetry for local containers
3. **Queue pipeline** — Dispatch via enqueue + `tm_wait_prev_tx()` for batch-friendly workloads; pending counter synchronizes dispatch with completion
4. **rbtree sentinel as valid array index** — `MAX_NODES - 1` with zero-initialized sentinel node instead of `-1`/`-2` to avoid out-of-bounds UB
5. **Non-void `txn_contains()` returns bool** — Key design point: 80% read operations use a non-void function, so `tm-instrument` cannot elide the call to an out-of-line clone; it must produce a real return value, keeping dispatch overhead identical across pipelines
6. **Total-store-ordering (TSO) assumed** — ARM64 backend limitations documented in SwissTM analysis; TSO memory model relied upon for correctness

## Next Steps
1. Produce paper plots from bank and rbtree benchmark results across all 3 pipelines
2. Benchmark on larger thread counts and other datastructures (AVL tree, hashmap)
3. Consider which pipeline analysis to include in the paper — queue advantages on bank are significant (27–37% faster, 62% fewer aborts), rbtree is neutral
4. Execute improvement plan (IMPROVEMENT_PLAN.md): fix remaining simulation spin loops, expand CI/CD, add more simulator backends, fix C++↔simulator address mismatch

## Critical Context
- Bank benefits substantially from queue pipeline (abort reduction via serialized commit order); rbtree is neutral because reads dominate and all pipelines inline the non-void `txn_contains()` call
- The 5-pass Honorio pipeline is the architectural foundation enabling all three variants
- LLVM 22+ required; macOS/Linux both supported
- Build system is Makefile-based with `tm_pipeline.mk` providing reusable compilation rules

## Relevant Files
- **`plugin/passes/TMInstrumentPass.cpp`** — Pipeline selection and enqueue/replace logic for queue variant
- **`plugin/passes/TMInstrumentFn.cpp`** — Function-level instrumentation
- **`plugin/tm_pipeline.mk`** — Pipeline build system (variants, backends, steps)
- **`benchmarks/plugin/datastructures/rbtree.cpp`** — Red-black tree benchmark (node pool with TM-annotated arrays)
- **`benchmarks/plugin/bank/`** — Bank transfer benchmark
- **`backends/tm_impl/`** — Backend implementations (TinySTM, TL2, NOrec, etc.)
- **`benchmark_results/`** — CSV results and plot scripts
