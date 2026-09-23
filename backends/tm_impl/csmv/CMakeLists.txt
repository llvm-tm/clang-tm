# ── CSMV Backend ──────────────────────────────────────────────────
# Multi-Versioned STM for GPUs (IPDPS 2022, JPDC 2023)
#
# CPU-only fallback: requires only a C++17 compiler
# Full GPU backend: requires CUDA Toolkit 12.0+
#
# Note: the GPU kernels live in gpu/backends/csmv/ (top-level gpu/ tree).

set(CSMV_GPU_DIR ${CMAKE_SOURCE_DIR}/gpu/backends/csmv)

option(BUILD_CSMV "Build CSMV GPU STM backend" OFF)
option(CSMV_CPU_FALLBACK "Build CSMV CPU-only fallback (no GPU)" ON)

if(NOT BUILD_CSMV AND NOT CSMV_CPU_FALLBACK AND NOT BUILD_CSMV_HIP)
    return()
endif()

# ── CPU fallback library ───────────────────────────────────────
add_library(csmv_cpu_backend STATIC
    cpu/csmv_cpu_runtime.cpp
    ${CMAKE_SOURCE_DIR}/backends/tm_impl/tm_region_allocator/tm_region_allocator.cpp
)

target_include_directories(csmv_cpu_backend
    PUBLIC  include
    PRIVATE ${CMAKE_SOURCE_DIR}/backends/tm_impl/common
    PRIVATE ${CMAKE_SOURCE_DIR}/backends/tm_impl/tm_region_allocator
)

target_link_libraries(csmv_cpu_backend PRIVATE
    pthread
)

# ── GPU kernel (optional) ──────────────────────────────────────
if(BUILD_CSMV AND CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)

    add_library(csmv_gpu_backend STATIC
        ${CSMV_GPU_DIR}/cuda/csmv_kernel.cu
    )

    target_include_directories(csmv_gpu_backend
        PUBLIC  include
        PRIVATE ${CMAKE_SOURCE_DIR}/backends/tm_impl/common
        PRIVATE ${CSMV_GPU_DIR}/cuda ${CSMV_GPU_DIR}/include
    )

    set_target_properties(csmv_gpu_backend PROPERTIES
        CUDA_ARCHITECTURES "70;75;80;90"
    )

    target_link_libraries(csmv_gpu_backend PRIVATE
        csmv_cpu_backend
        CUDA::cudart
    )
endif()

# ── HIP backend (optional) ───────────────────────────────────────
option(BUILD_CSMV_HIP "Build CSMV GPU backend with HIP" OFF)

if(BUILD_CSMV_HIP)
    find_package(hip REQUIRED)
    enable_language(HIP)
    # .cu defaults to the CUDA language; route it to HIP (hipcc) explicitly.
    set_source_files_properties(
        ${CSMV_GPU_DIR}/cuda/csmv_kernel.cu
        ${CSMV_GPU_DIR}/cuda/csmv_batch_executor.cu
        PROPERTIES LANGUAGE HIP)

    add_library(csmv_gpu_backend_hip STATIC
        ${CSMV_GPU_DIR}/cuda/csmv_kernel.cu
        ${CSMV_GPU_DIR}/cuda/csmv_batch_executor.cu
    )

    target_include_directories(csmv_gpu_backend_hip
        PUBLIC  include
        PRIVATE ${CMAKE_SOURCE_DIR}/backends/tm_impl/common
        PRIVATE ${CSMV_GPU_DIR}/cuda ${CSMV_GPU_DIR}/include
    )

    target_compile_options(csmv_gpu_backend_hip PRIVATE -x hip)

    target_link_libraries(csmv_gpu_backend_hip PRIVATE
        csmv_cpu_backend
    )
endif()
