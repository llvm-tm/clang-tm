# GPU Transactional Memory

GPU kernels, host TM hooks, and benchmarks for GPU transactional memory
backends.  CPU-only fallbacks live in `backends/tm_impl/` (under the
standard hook API) so the CPU test suite still covers them; this directory
holds everything that requires a CUDA/HIP toolchain.

## Layout

```
gpu/
├── backends/               one directory per GPU backend, unified shape:
│   ├── <name>/
│   │   ├── include/        public headers (API + batch-executor headers used by benchmarks)
│   │   └── cuda/           device/host implementation (.cu/.cuh/.cpp)
│   ├── csmv/               csmv_kernel.{cu,cuh} + csmv_batch_executor.{cu} / include: .hpp
│   ├── gpu_stm/            pr_stm_kernel.cuh, pr_stm_host.cpp, pr_stm_runtime.cu
│   ├── gpu_gputx/          GPUTX kernels (priority concurrency control)
│   ├── gpu_gacco/          GACCO kernels
│   └── gpu_gust/           GUST kernels (MVCC, versioned boxes + commit log)
└── benchmarks/             ALL drivers live here — gpu_ycsb, gpu_kmeans, gpu_memcached,
                            gpu_tpcc, gpu_fuzz_counter, gpu_bank, gpu_ycsb_gust,
                            gpu_memcached_gust, gpu_gust_smoke (test driver, moved out
                            of the backend dir in the review-05 layout pass)
```

Rule: benchmark/test drivers never live inside a backend directory; backend
directories only contain protocol + host glue.

## CPU fallbacks (kept in backends/tm_impl/)

- `backends/tm_impl/gpu_stm/cpu/` — PR-STM CPU fallback (`gpu_stm_cpu` backend)
- `backends/tm_impl/csmv/cpu/` — CSMV CPU fallback
- `backends/tm_impl/gputx/` — GPUTX CPU-only backend (priority CC, no GPU)

Shared headers (public APIs consumed by both CPU and GPU code) stay in
`backends/tm_impl/<name>/include/`; headers used only by GPU code live in
`gpu/backends/<name>/include/`. The CUDA/HIP platform shim is
`backends/tm_impl/common/tm_gpu_platform.hpp`.

## Building

The GPU benchmarks are built with `make -C gpu/benchmarks` (requires `nvcc`;
for AMD use `make HIP=1` — hipcc detection is explicit, and the Makefile
fails loudly if neither is present). CMake: `BUILD_GPU_STM` is wired via the
top-level `CMakeLists.txt`; the `BUILD_CSMV*` options in
`backends/tm_impl/csmv/CMakeLists.txt` exist but are not yet `add_subdirectory`'d
(review-05 T-02/T-03 track fixing or removing them).
