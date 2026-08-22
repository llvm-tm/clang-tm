# GPU Transactional Memory

GPU kernels, host TM hooks, and benchmarks for GPU transactional memory
backends.  CPU-only fallbacks live in `backends/tm_impl/` (under the
standard hook API) so the CPU test suite still covers them; this directory
holds everything that requires a CUDA/HIP toolchain.

## Layout

```
gpu/
├── backends/
│   ├── csmv/          CSMV kernels (csmv_kernel.{cu,cuh}, csmv_batch_executor.{cu,hpp})
│   ├── gpu_stm/       PR-STM kernels (pr_stm_kernel.cuh, pr_stm_host.cpp, pr_stm_runtime.cu)
│   ├── gpu_gputx/     GPUTX GPU kernels (priority concurrency control)
│   ├── gpu_gacco/     GACCO GPU kernels
│   └── gpu_gust/      GUST GPU kernels (MVCC, versioned boxes + commit log)
└── benchmarks/        gpu_ycsb, gpu_kmeans, gpu_memcached, gpu_tpcc, gpu_fuzz_counter
```

## CPU fallbacks (kept in backends/tm_impl/)

- `backends/tm_impl/gpu_stm/cpu/` — PR-STM CPU fallback (`gpu_stm_cpu` backend)
- `backends/tm_impl/csmv/cpu/` — CSMV CPU fallback
- `backends/tm_impl/gputx/` — GPUTX CPU-only backend (priority CC, no GPU)

Shared headers (public APIs) stay in `backends/tm_impl/<name>/include/`;
the CUDA/HIP platform shim is `backends/tm_impl/common/tm_gpu_platform.hpp`.

## Building

The GPU benchmarks are built with `make -C gpu/benchmarks` (auto-detects
nvcc/hipcc; `HIP=1 HIPCC=...` for AMD).  CMake users can enable
`BUILD_GPU_STM`/`GPU_STM_CPU_FALLBACK` and `BUILD_CSMV`/`CSMV_CPU_FALLBACK`
via the top-level `CMakeLists.txt`.
