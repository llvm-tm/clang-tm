# ── CSMV Backend ──────────────────────────────────────────────────
# Multi-Versioned STM for GPUs (IPDPS 2022, JPDC 2023)
#
# CPU-only fallback: requires only a C++17 compiler
# Full GPU backend: requires CUDA Toolkit 12.0+

option(BUILD_CSMV "Build CSMV GPU STM backend" OFF)
option(CSMV_CPU_FALLBACK "Build CSMV CPU-only fallback (no GPU)" ON)

if(NOT BUILD_CSMV AND NOT CSMV_CPU_FALLBACK)
    return()
endif()

# ── CPU fallback library ───────────────────────────────────────
add_library(csmv_cpu_backend STATIC
    cpu/csmv_cpu_runtime.cpp
)

target_include_directories(csmv_cpu_backend
    PUBLIC  include
    PRIVATE ${CMAKE_SOURCE_DIR}/backends/tm_impl/common
)

target_link_libraries(csmv_cpu_backend PRIVATE
    tm_region_allocator
    pthread
)

# ── GPU kernel (optional) ──────────────────────────────────────
if(BUILD_CSMV AND CMAKE_CUDA_COMPILER)
    enable_language(CUDA)

    add_library(csmv_gpu_backend STATIC
        gpu/csmv_kernel.cu
    )

    target_include_directories(csmv_gpu_backend
        PUBLIC  include
        PRIVATE ${CMAKE_SOURCE_DIR}/backends/tm_impl/common
    )

    set_target_properties(csmv_gpu_backend PROPERTIES
        CUDA_ARCHITECTURES "70;75;80;90"
    )

    target_link_libraries(csmv_gpu_backend PRIVATE
        csmv_cpu_backend
        cuda
    )
endif()

# ── HIP backend (optional) ───────────────────────────────────────
option(BUILD_CSMV_HIP "Build CSMV GPU backend with HIP" OFF)

if(BUILD_CSMV_HIP)
    find_package(hip REQUIRED)

    add_library(csmv_gpu_backend_hip STATIC
        gpu/csmv_kernel.cu
        gpu/csmv_batch_executor.cu
    )

    target_include_directories(csmv_gpu_backend_hip
        PUBLIC  include
        PRIVATE ${CMAKE_SOURCE_DIR}/backends/tm_impl/common
    )

    target_compile_options(csmv_gpu_backend_hip PRIVATE -x hip)

    target_link_libraries(csmv_gpu_backend_hip PRIVATE
        csmv_cpu_backend
    )
endif()
