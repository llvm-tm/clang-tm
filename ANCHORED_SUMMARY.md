# TM API — Anchored Summary

## Goal
A multi-paradigm C++ transactional memory framework (LLVM plugin, explicit C++ API, Rust bindings) with three instrumentation pipelines, supporting 16+ STM/HTM/distributed TM backends, targeting a paper submission.

## Constraints & Preferences
- **LLVM 22+** plugin using the 5-pass Honorio decomposition (Clone → Collect → Redirect → InstrumentFn → Cleanup)
- Three pipeline variants: `tm-instrument` (standard), `tm-instrument-inline` (inline-then-instrument), `tm-instrument-queue` (queue-based dispatch)
- All backends use `TMRealHooks` + `tm_register_real_hooks()` for both plugin and explicit API paths
- Backends: TinySTM (wbctl/wbetl/wt), TL2, NOrec, SwissTM, SingleLock, TSXSGL, PersistentSGL, DistributedSGL, DUDETM, SPHT, NVHTM, XTM, Romulus, LeftRight, GPU_STM_CPU, TiKV
- Benchmarks: bank, rbtree, STAMP (7 benchmarks), STMbench7, EigenBench, TPC-C, YCSB, DeathStarBench social_tm

## Progress

### Done
- ✓ **5-pass Honorio pipeline** — TMInstrumentPass decomposed into ClonePass, CollectPass, RedirectPass, InstrumentFnPass, CleanupPass with `tm_allow_opaque` semantics
- ✓ **Three pipeline variants all working** — `tm-instrument`, `tm-instrument-inline`, `tm-instrument-queue` produce correct binaries
- ✓ **rbtree benchmark bugs fixed** — sentinel UB, insert duplicate, pool bounds checks
- ✓ **bank benchmarked on all 3 pipelines** — Queue pipeline 27–37% faster, 62% fewer aborts
- ✓ **GPU_STM_CPU backend (PR-STM)**: new priority-based OCC backend with shared lock table + global clock. `test_tx` 114/114, `test_ds` 207/207. TLA+ model verified (13 states, all invariants PASS).
- ✓ **GPU_PR_STM TLA+ model** (`docs/proofs/GPU_PR_STM.tla`): warp-cooperative TM with per-warp commit counter, `InvCommitBudget` invariant
- ✓ **CMake integration**: `BUILD_GPU_STM`/`GPU_STM_CPU_FALLBACK` options, `add_subdirectory(gpu_stm)`, LLVM 22→22.1 version fix
- ✓ **PR-STM correctness fixes**: write-back phase, read-own-writes, find_write backwards, lock spin-loop, priority comparison, `__ATOMIC_ACQUIRE` semantics
- ✓ **Linkage bug fix**: Added `extern "C"` to lifecycle functions in non-plugin paths
- ✓ **Pre-existing bug fixes**: `std::barrier`→`SpinBarrier`, lock word mask, `goto abort_tx`, `__CUDACC__` guard
- ✓ **Queue/async pipeline working**: plugin on LLVM 22.1.6, `test_queue`/`test_queue_multi` PASS, `bench_queue_compare2_queue` fixed
- ✓ **CSMV backend (Multi-Versioned STM for GPUs)**: new backend implementing IPDPS 2022 / JPDC 2023 paper. Version-list-based MV-STM — reads NEVER abort. CPU fallback via TMRealHooks (csmv_cpu_runtime.cpp), CUDA kernel with warp-cooperative traversal (csmv_kernel.cu), PlusCal TLA+ model (CSMV.tla) with ReadConsistencyOK + VersionChainMonotonicOK + NoConcurrentLocks + FenceFidelityOK invariants. `test_tx` 114/114, `test_ds` 207/207.
- ✓ **Comprehensive implementations document** (`docs/IMPLEMENTATIONS.md`): 17 backends covered with algorithm description, key files, verification matrix, Rust coverage, TLA+ status
- ✓ **Plugin STAMP** all 8 benchmarks × 3 backends × 2 thread counts = 48/48 PASS
- ✓ **LLVM_TM_PLUGIN guards** for all 15+ backends
- ✓ **NOrec fix** (2026-06-23): `-DLLVM_TM_PLUGIN` in `clang-tm` script
- ✓ **DeathStarBench social_tm**: new benchmark — TinySTM WBCTL/WT pass invariant
- ✓ **TLA+ liveness sweep**: all 18 backends have liveness configs, PlusCal conversions for NOrec, DUDETM, NVHTM, SPHT, TiKV, DESEngine
- ✓ **Simulator cost mode**: SimEngine + machine profile calibration, 26/26 simulator tests pass

### In Progress
- **Bank multi-threaded** GPU_STM_CPU shows money creation under contention — lock table hash collision / priority acquire under investigation
- **NOrec plugin bypass bug**: `#ifdef LLVM_TM_PLUGIN` skips TM tracking for non-TM-region addresses (heap `new`/`malloc`). Need to replace with `LLVM_TM_ADDR_CHECK` pattern like TinySTM
- Paper writing and results collection

### Blocked
- None

## Key Decisions
1. **5-pass decomposition** — Split monolithic TMInstrumentPass into Clone → Collect → Redirect → InstrumentFn → Cleanup for modularity and correctness
2. **Pipeline default is `tm-instrument`** (non-inline) — avoids write-set/memory asymmetry for local containers
3. **Queue pipeline** — Dispatch via enqueue + `tm_wait_prev_tx()` for batch-friendly workloads; pending counter synchronizes dispatch with completion
4. **All hooks are DATA variables** (function pointers) — enables stub/real swap, phase-based TM, and immediate effect for LLVM plugin indirect calls
5. **`extern "C"` on lifecycle functions** — must match `tm_api.hpp` declarations (TinySTM wraps entire file in `extern "C" {}`)
6. **GPU_STM_CPU uses PR-STM** — priority-based OCC without warp barriers (each thread independent via TMRealHooks)
7. **NOrec plugin bypass is too broad** — will be replaced with `LLVM_TM_ADDR_CHECK` (stack-only bypass) matching TinySTM's pattern

## Next Steps
1. Fix NOrec plugin bypass bug (replace `isTMAddress()` with `LLVM_TM_ADDR_CHECK`)
2. Fix PR-STM bank money creation (lock table collision handling)
3. Produce paper plots from bank and rbtree benchmark results across all 3 pipelines
4. Benchmark on larger thread counts and other datastructures (AVL tree, hashmap)
5. Complete remaining PlusCal conversions (DistributedSGL, TSXSim)

## Critical Context
- Bank benefits substantially from queue pipeline (abort reduction via serialized commit order); rbtree is neutral because reads dominate and all pipelines inline the non-void `txn_contains()` call
- The 5-pass Honorio pipeline is the architectural foundation enabling all three variants
- LLVM 22+ required; macOS/Linux both supported
- Build system is Makefile-based with `tm_pipeline.mk` providing reusable compilation rules
- **NOrec has a known correctness gap in plugin mode** — non-TM-region addresses are bypassed entirely (no STM tracking)
- **GPU_STM_CPU** is the 16th backend; TiKV is the 17th (distributed)

## Relevant Files
- **`backends/tm_impl/gpu_stm/cpu/gpu_stm_cpu_runtime.cpp`** — PR-STM CPU fallback
- **`backends/tm_impl/gpu_stm/include/gpu_stm_api.h`** — PR-STM lock word encoding
- **`backends/tm_impl/csmv/cpu/csmv_cpu_runtime.cpp`** — CSMV CPU fallback (MV-STM, reads never abort)
- **`backends/tm_impl/csmv/include/csmv_api.h`** — CSMV version node + object entry structures
- **`gpu/backends/csmv/csmv_kernel.cu`** — CSMV CUDA kernel (warp-cooperative traversal)
- **`backends/tm_impl/csmv/CMakeLists.txt`** — CSMV CMake build (CSMV_CPU_FALLBACK / BUILD_CSMV)
- **`docs/proofs/CSMV.tla`** — PlusCal model with ReadConsistencyOK + VersionChainMonotonicOK invariants
- **`docs/proofs/GPU_PR_STM.tla`** — TLA+ model for PR-STM
- **`docs/IMPLEMENTATIONS.md`** — Comprehensive backend reference (17 backends)
- **`backends/tm_impl/tiny_stm/TinySTM_runtime.cpp`** — Reference backend (extern "C" wrapping pattern)
- **`backends/tm_impl/common/tm_hooks.hpp`** — TMRealHooks registration system
- **`backends/tm_impl/common/tm_hooks.cpp`** — Shared TLS variables
- **`backends/tm_impl/common/tm_common.hpp`** — `LLVM_TM_ADDR_CHECK` macros
- **`backends/tm_impl/gpu_stm/README.md`** — GPU STM README updated with CSMV comparison
- **`plugin/clang-tm`** — Added `-DLLVM_TM_PLUGIN` default define
