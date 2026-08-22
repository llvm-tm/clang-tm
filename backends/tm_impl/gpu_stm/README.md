# GPU STM Backend — PR-STM

## Cross-platform GPU compute: state of the art (2026)

| API | Vendors | Language | Portability | Maturity | TM-suitability |
|-----|---------|----------|-------------|----------|----------------|
| **CUDA** | NVIDIA only | CUDA C++ | None | ★★★★★ | ★★★★★ (warp primitives, atomicCAS, __threadfence) |
| **HIP** | AMD + NVIDIA | HIP C++ | NVIDIA+AMD via hipify | ★★★★☆ | ★★★★☆ (same primitives as CUDA) |
| **SYCL** (AdaptiveCpp) | NVIDIA, AMD, Intel, CPU | C++17 single-source | Widest cross-vendor | ★★★★☆ | ★★★☆☆ (sub-group ops maturing) |
| **Vulkan Compute** | All (incl mobile) | GLSL/HLSL→SPIR-V | Widest driver coverage | ★★★★☆ | ★★☆☆☆ (verbose, no warp primitives) |
| **OpenCL 3.0** | All (incl FPGAs) | C99 kernels | Broad but stagnating | ★★★★☆ | ★★☆☆☆ (no warp, deprecated by vendors) |
| **Metal 4** | Apple only | MSL | Apple only | ★★★★★ | ★★★☆☆ (unified memory is nice) |

### Recommendation: SYCL (AdaptiveCpp) for portability + CUDA for maturity

**SYCL** is the strongest cross-platform answer for 2026 greenfield GPU compute:
- Single-source C++ (no separate kernel language — same file as host code)
- Targets NVIDIA, AMD, Intel, and CPU from one codebase
- SYCL 2020 standard with sub-group operations (`ballot`, `any_of`, `reduce`)
- AdaptiveCpp (formerly hipSYCL) supports CUDA, HIP, OpenCL, and CPU backends
- Intel's SYCLomatic tool auto-translates CUDA→SYCL (70–90% coverage)
- Best performance portability metrics among portable models (ICS 2025)

**HIP** is the pragmatic choice if your target is AMD + NVIDIA:
- Syntactically nearly identical to CUDA (same kernel model, same warp primitives)
- `hipify` tool auto-translates CUDA source
- ROCm 7 (Sept 2025) added native Windows support
- Best performance on AMD hardware

**This implementation uses CUDA** (only GPU toolchain available on this system)
but is structured for portability. See portability notes below.

## PR-STM Algorithm

PR-STM (Priority Rule STM for GPUs, Shen et al. 2015):

- **Lock-based, commit-time validation** — similar to TL2 but adapted for SIMT
- **32-bit lock word**: encodes [priority (8b) | version (23b) | locked (1b)]
- **Static thread priorities**: warp ID determines priority; higher priority steals locks
- **Warp-level abort**: if any lane detects conflict, entire warp aborts together
- **Encounter-time lock sorting**: prevent deadlock within warp writes

### Transaction phases (matched to PlusCal model in docs/proofs/GPU_PR_STM.tla)

```
L_begin:   snapshot global clock, clear read/write sets
L_read:    each lane reads addresses, records versions
L_write:   each lane buffers writes
L_validate: all lanes check read-set consistency; warp any_of detects conflict
L_lock:    priority-based atomicCAS on lock table; abort if higher-priority holder
L_commit:  threadfence, write-back, release locks with new version
L_abort:   clear state, retry
```

## Architecture

```
                       Host (CPU)
┌──────────────────────────────────────────────────────────┐
│  tm_init()       → cudaMalloc lock table + global clock   │
│  tm_begin()      → per-thread state init (CPU PR-STM)    │
│  tm_read/tm_write → CPU emulation or kernel dispatch      │
│  tm_commit()     → cudaLaunchCooperativeKernel (GPU) or  │
│                     CPU PR-STM commit (no-GPU fallback)   │
│  tm_exit()       → cudaFree, cudaDeviceReset             │
└─────────────────────────┬────────────────────────────────┘
                          │ cudaMallocManaged / cudaMemcpy
┌─────────────────────────▼────────────────────────────────┐
│                     Device (GPU)                          │
│  Persistent kernel or launch-per-tx:                      │
│    blockDim.x = WARP_SIZE (32)                            │
│    gridDim.x  = NUM_WARPS                                 │
│  Shared memory: read-set/write-set per lane               │
│  Global memory: lock table + data                         │
│  Warp primitives: __ballot_sync, __any_sync, __syncwarp   │
└──────────────────────────────────────────────────────────┘
```

## Portability notes: CUDA → SYCL → HIP

### Warp-level primitives

| CUDA | SYCL (AdaptiveCpp) | HIP |
|------|-------------------|-----|
| `__ballot_sync(mask, pred)` | `sg.ballot(pred)` | `__ballot_sync(mask, pred)` |
| `__any_sync(mask, pred)` | `sycl::any_of_group(sg, pred)` | `__any_sync(mask, pred)` |
| `__syncwarp(mask)` | `sycl::group_barrier(sg)` | `__syncwarp(mask)` |
| `__shfl_sync(mask, val, lane)` | `sg.shuffle(val, lane)` | `__shfl_sync(mask, val, lane)` |
| `threadIdx.x % warpSize` | `sg.get_local_id()` | `hipThreadIdx_x % warpSize` |
| `warpSize` | `sg.get_local_range()` | `warpSize` |

### Memory operations

| CUDA | SYCL | HIP |
|------|------|-----|
| `atomicCAS(addr, cmp, val)` | `sycl::atomic_ref{*addr}.compare_exchange_strong(cmp, val)` | `atomicCAS(addr, cmp, val)` |
| `__threadfence()` | `sycl::atomic_fence(sycl::memory_order::seq_cst)` | `__threadfence()` |
| `__threadfence_block()` | `sycl::atomic_fence(...)` on work-group | `__threadfence_block()` |
| `cudaMallocManaged` | `sycl::malloc_shared` | `hipMallocManaged` |
| `__shared__` | `sycl::local_accessor` | `__shared__` |

### Device management

| CUDA | SYCL | HIP |
|------|------|-----|
| `cudaSetDevice` | `sycl::device_selector` | `hipSetDevice` |
| `cudaStream_t` | `sycl::queue` | `hipStream_t` |
| `cudaEvent_t` | `sycl::event` | `hipEvent_t` |

## Related GPU STM Work

### CSMV — Multi-Versioned STM for GPUs

**CSMV** (IPDPS 2022 / JPDC 2023, Nunes, Castro, Romano) is a multi-versioned STM for
GPUs. Unlike PR-STM (single-version priority-based OCC), CSMV retains multiple versions
per object so readers NEVER abort — they always find a consistent snapshot. The tradeoff
is higher memory overhead and the need for version GC.

A CSMV backend (CPU fallback + CUDA kernel) is available at `backends/tm_impl/csmv/`.
Both PR-STM and CSMV use the same `TMRealHooks` interface and can be swapped at runtime.

| Feature | PR-STM | CSMV |
|---------|--------|------|
| Versioning | Single-version (OCC) | Multi-version |
| Reads abort? | Yes (priority conflict) | Never |
| Lock granularity | Lock word per stripe | Version list per stripe |
| GC needed? | No (inline write-back) | Yes (trim old versions) |
| GPU traversal | Per-lane lock CAS | Warp-cooperative list walk |

## Build

### Prerequisites
- CUDA Toolkit 12.0+ (or SYCL compiler for portable builds)
- CMake 3.18+ (for CUDA support)
- NVIDIA GPU with compute capability 7.0+ (Volta or newer)
- Or: CPU-only mode (no GPU required)

### CMake build (CUDA)
```sh
mkdir build && cd build
cmake .. -DBUILD_GPU_STM=ON -DCMAKE_CUDA_ARCHITECTURES=70
make -j$(nproc) pr_stm_backend
```

### CPU-only build (no GPU required)
```sh
cmake .. -DBUILD_GPU_STM=ON -DGPU_STM_CPU_FALLBACK=ON
make -j$(nproc) pr_stm_backend
```

## Files

| File | Description |
|------|-------------|
| `include/gpu_stm_api.h` | Public C API (TMRealHooks-compatible) |
| `gpu/backends/gpu_stm/pr_stm_kernel.cuh` | CUDA kernel: PR-STM warp-level algorithm |
| `gpu/backends/gpu_stm/pr_stm_runtime.cu` | Host runtime: CUDA device management, kernel launch |
| `cpu/pr_stm_cpu.cpp` | CPU fallback: std::thread-based PR-STM emulation |
| `CMakeLists.txt` | CUDA-enabled CMake build |
