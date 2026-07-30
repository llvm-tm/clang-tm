#pragma once

#include <cstdio>
#include <cstdlib>

// ── CUDA / HIP portability layer ──────────────────────────────
// Kernel keywords (__global__, __device__, __syncwarp, __ballot_sync,
// __threadfence, atomicCAS, etc.) are IDENTICAL between CUDA and HIP.
// Only host-side API names differ (cudaMalloc vs hipMalloc).
//
// Strategy: #define cuda* → hip* when compiling under HIP, so source
// files keep using cudaMalloc/cudaFree/... unchanged.  Under CUDA the
// same names resolve directly.  Zero changes to kernel code.
//
// Usage:
//   #include "tm_gpu_platform.hpp"   // replaces #include <cuda_runtime.h>
//   CUDA_CHECK(cudaMalloc(...));      // works on both platforms
//   #ifdef __CUDACC__                 // probably want TM_GPU_COMPILER

#if defined(__HIPCC__) || defined(__HIP_PLATFORM_AMD__) || defined(__HIP_PLATFORM_HCC__)

  #include <hip/hip_runtime.h>

  // ── Remap cuda* → hip* ───────────────────────────────────────
  // Source files keep calling cudaMalloc, cudaFree, etc. unchanged.
  // Under HIP these resolve to the hip* equivalents.
  #define cudaDeviceSynchronize    hipDeviceSynchronize
  #define cudaGetDeviceCount       hipGetDeviceCount
  #define cudaSetDevice            hipSetDevice
  #define cudaMalloc               hipMalloc
  #define cudaMallocManaged        hipMallocManaged
  #define cudaFree                 hipFree
  #define cudaMemset               hipMemset
  #define cudaMemcpy               hipMemcpy
  #define cudaMemcpyAsync          hipMemcpyAsync
  #define cudaMemcpyToSymbol       hipMemcpyToSymbol
  #define cudaMemcpyFromSymbol     hipMemcpyFromSymbol
  #define cudaDeviceReset          hipDeviceReset
  #define cudaGetLastError         hipGetLastError
  #define cudaGetErrorString       hipGetErrorString
  #define cudaStreamCreate         hipStreamCreate
  #define cudaStreamDestroy        hipStreamDestroy
  #define cudaStreamSynchronize    hipStreamSynchronize
  #define cudaEventCreate          hipEventCreate
  #define cudaEventDestroy         hipEventDestroy
  #define cudaEventRecord          hipEventRecord
  #define cudaEventSynchronize     hipEventSynchronize
  #define cudaEventElapsedTime     hipEventElapsedTime

  // ── Types ────────────────────────────────────────────────────
  #define cudaError_t              hipError_t
  #define cudaSuccess              hipSuccess
  #define cudaStream_t             hipStream_t
  #define cudaEvent_t              hipEvent_t
  #define cudaMemcpyKind           hipMemcpyKind
  #define cudaMemcpyHostToDevice   hipMemcpyHostToDevice
  #define cudaMemcpyDeviceToHost   hipMemcpyDeviceToHost

  #define TM_GPU_PLATFORM "HIP"

#elif defined(__CUDACC__)

  #include <cuda_runtime.h>

  #define TM_GPU_PLATFORM "CUDA"

#elif defined(TM_GPU_USE_HIP)

  // Manual HIP override: host code linked against HIP runtime.
  // #define TM_GPU_USE_HIP before including this header when
  // compiling with a regular C++ compiler that links to HIP.
  #include <hip/hip_runtime.h>

  #define cudaDeviceSynchronize    hipDeviceSynchronize
  #define cudaGetDeviceCount       hipGetDeviceCount
  #define cudaSetDevice            hipSetDevice
  #define cudaMalloc               hipMalloc
  #define cudaMallocManaged        hipMallocManaged
  #define cudaFree                 hipFree
  #define cudaMemset               hipMemset
  #define cudaMemcpy               hipMemcpy
  #define cudaMemcpyAsync          hipMemcpyAsync
  #define cudaMemcpyToSymbol       hipMemcpyToSymbol
  #define cudaMemcpyFromSymbol     hipMemcpyFromSymbol
  #define cudaDeviceReset          hipDeviceReset
  #define cudaGetLastError         hipGetLastError
  #define cudaGetErrorString       hipGetErrorString
  #define cudaStreamCreate         hipStreamCreate
  #define cudaStreamDestroy        hipStreamDestroy
  #define cudaStreamSynchronize    hipStreamSynchronize
  #define cudaEventCreate          hipEventCreate
  #define cudaEventDestroy         hipEventDestroy
  #define cudaEventRecord          hipEventRecord
  #define cudaEventSynchronize     hipEventSynchronize
  #define cudaEventElapsedTime     hipEventElapsedTime

  #define cudaError_t              hipError_t
  #define cudaSuccess              hipSuccess
  #define cudaStream_t             hipStream_t
  #define cudaEvent_t              hipEvent_t
  #define cudaMemcpyKind           hipMemcpyKind
  #define cudaMemcpyHostToDevice   hipMemcpyHostToDevice
  #define cudaMemcpyDeviceToHost   hipMemcpyDeviceToHost

  #define TM_GPU_PLATFORM "HIP"

#else
  #error "tm_gpu_platform.hpp requires CUDA (__CUDACC__) or HIP (__HIPCC__), or define TM_GPU_USE_HIP for host-only HIP code"
#endif

// ── Unified error-checking macro ───────────────────────────────
// CUDA_CHECK works under both platforms because cudaGetErrorString
// and cudaSuccess are remapped in the HIP path.
#define CUDA_CHECK(call) do { \
    cudaError_t _err_ = call; \
    if (_err_ != cudaSuccess) { \
        fprintf(stderr, TM_GPU_PLATFORM " error %d at %s:%d: %s\n", \
                (int)_err_, __FILE__, __LINE__, cudaGetErrorString(_err_)); \
        abort(); \
    } \
} while(0)

// ── Unified compiler guard ─────────────────────────────────────
#if defined(__CUDACC__) || defined(__HIPCC__)
  #define TM_GPU_COMPILER 1
#else
  #define TM_GPU_COMPILER 0
#endif
