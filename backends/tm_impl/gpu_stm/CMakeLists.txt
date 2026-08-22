# GPU STM Backend CMakeLists
#
# Provides:
#   pr_stm_backend       — CUDA kernel + host runtime (requires NVIDIA GPU)
#   pr_stm_backend_cpu   — CPU-only fallback (std::thread, no GPU required)
#
# Portability:
#   Replace `enable_language(CUDA)` with `find_package(SYCL)` or
#   `find_package(HIP)` to target AMD/Intel GPUs.
#   See README.md for SYCL/HIP portability mapping.

cmake_minimum_required(VERSION 3.18)

option(BUILD_GPU_STM "Build GPU STM backend (PR-STM)" OFF)
option(GPU_STM_CPU_FALLBACK "Build CPU-only fallback (no GPU)" OFF)

if(NOT BUILD_GPU_STM AND NOT GPU_STM_CPU_FALLBACK)
    return()
endif()

# ── Include paths ──────────────────────────────────────────────────

set(GPU_STM_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR})
set(GPU_STM_INC_DIR  ${GPU_STM_SRC_DIR}/include)
set(BACKENDS_COMMON  ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(BACKENDS_REGION  ${CMAKE_CURRENT_SOURCE_DIR}/../tm_region_allocator)
set(GPU_STM_CUDA_DIR ${CMAKE_SOURCE_DIR}/gpu/backends/gpu_stm)

include_directories(${GPU_STM_INC_DIR})
include_directories(${BACKENDS_COMMON})
include_directories(${BACKENDS_REGION})
include_directories(${GPU_STM_CUDA_DIR})

# ── CPU-only fallback ──────────────────────────────────────────────

if(GPU_STM_CPU_FALLBACK)
    add_library(pr_stm_backend_cpu STATIC
        ${GPU_STM_SRC_DIR}/cpu/gpu_stm_cpu_runtime.cpp
        ${BACKENDS_REGION}/tm_region_allocator.cpp
        ${BACKENDS_COMMON}/tm_hooks.cpp
    )
    target_compile_features(pr_stm_backend_cpu PUBLIC cxx_std_17)
    target_include_directories(pr_stm_backend_cpu PUBLIC
        ${GPU_STM_INC_DIR}
        ${BACKENDS_COMMON}
        ${BACKENDS_REGION}
    )
    message(STATUS "GPU STM: CPU fallback library enabled")

    # CPU emulation smoke test (standalone PR-STM, not TMRealHooks)
    add_executable(test_pr_stm_cpu
        ${GPU_STM_SRC_DIR}/cpu/pr_stm_cpu.cpp
        tests/test_pr_stm.cpp
    )
    target_compile_features(test_pr_stm_cpu PUBLIC cxx_std_17)
    target_include_directories(test_pr_stm_cpu PUBLIC
        ${GPU_STM_INC_DIR}
        ${BACKENDS_COMMON}
        ${BACKENDS_REGION}
    )
    target_link_libraries(test_pr_stm_cpu PRIVATE pthread)
    message(STATUS "GPU STM: CPU emulation test enabled")
endif()

# ── CUDA backend ───────────────────────────────────────────────────

if(BUILD_GPU_STM)
    enable_language(CUDA)

    # Detect CUDA architecture
    set(CMAKE_CUDA_ARCHITECTURES "70;75;80;90" CACHE STRING
        "CUDA architectures to compile for")

    # PR-STM kernel + host runtime
    add_library(pr_stm_backend STATIC
        ${GPU_STM_CUDA_DIR}/pr_stm_runtime.cu
        ${BACKENDS_REGION}/tm_region_allocator.cpp
        ${BACKENDS_COMMON}/tm_hooks.cpp
    )
    target_compile_features(pr_stm_backend PUBLIC cxx_std_17)
    target_include_directories(pr_stm_backend PUBLIC
        ${GPU_STM_INC_DIR}
        ${BACKENDS_COMMON}
        ${BACKENDS_REGION}
    )
    target_link_libraries(pr_stm_backend PUBLIC
        cudart_static::cudart_static
    )

    # ── Test: PR-STM GPU smoke test ───────────────────────────────
    add_executable(test_pr_stm_gpu
        ${GPU_STM_CUDA_DIR}/pr_stm_runtime.cu
        tests/test_pr_stm.cpp
    )
    target_compile_features(test_pr_stm_gpu PUBLIC cxx_std_17)
    target_include_directories(test_pr_stm_gpu PUBLIC
        ${GPU_STM_INC_DIR}
        ${BACKENDS_COMMON}
        ${BACKENDS_REGION}
    )
    target_link_libraries(test_pr_stm_gpu PRIVATE
        cudart_static::cudart_static
    )

    message(STATUS "GPU STM: CUDA backend enabled")
    message(STATUS "  CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")
endif()

option(BUILD_GPU_STM_HIP "Build GPU STM backend with HIP" OFF)

if(BUILD_GPU_STM_HIP)
    find_package(hip REQUIRED)
    message(STATUS "GPU STM: HIP backend enabled")

    # Host-side code compiled with regular C++ (g++/clang++)
    add_library(pr_stm_backend_hip_host STATIC
        ${GPU_STM_CUDA_DIR}/pr_stm_host.cpp
        ${BACKENDS_REGION}/tm_region_allocator.cpp
        ${BACKENDS_COMMON}/tm_hooks.cpp
    )
    target_compile_features(pr_stm_backend_hip_host PUBLIC cxx_std_17)
    target_include_directories(pr_stm_backend_hip_host PUBLIC
        ${GPU_STM_INC_DIR}
        ${BACKENDS_COMMON}
        ${BACKENDS_REGION}
        ${HIP_PATH}/include
    )
    target_link_libraries(pr_stm_backend_hip_host PRIVATE pthread)

    # Device-side kernel compiled with hipcc
    add_library(pr_stm_backend_hip_kernel STATIC
        ${GPU_STM_CUDA_DIR}/pr_stm_runtime.cu
    )
    target_compile_features(pr_stm_backend_hip_kernel PUBLIC cxx_std_17)
    target_include_directories(pr_stm_backend_hip_kernel PUBLIC
        ${GPU_STM_INC_DIR}
        ${BACKENDS_COMMON}
        ${BACKENDS_REGION}
    )

    # Combined library
    add_library(pr_stm_backend_hip INTERFACE)
    target_link_libraries(pr_stm_backend_hip INTERFACE
        pr_stm_backend_hip_host
        pr_stm_backend_hip_kernel
    )

    add_executable(test_pr_stm_gpu_hip
        ${GPU_STM_CUDA_DIR}/pr_stm_host.cpp
        ${GPU_STM_CUDA_DIR}/pr_stm_runtime.cu
        tests/test_pr_stm.cpp
    )
    target_compile_features(test_pr_stm_gpu_hip PUBLIC cxx_std_17)
    target_include_directories(test_pr_stm_gpu_hip PUBLIC
        ${GPU_STM_INC_DIR}
        ${BACKENDS_COMMON}
        ${BACKENDS_REGION}
    )
    target_link_libraries(test_pr_stm_gpu_hip PRIVATE pthread)
endif()


