// GPU EPCC kernel launch wrapper
#include "tm_gpu_platform.hpp"
#include "gpu_epcc_api.h"
#include "gpu_epcc_kernel.cuh"

int gpu_epcc_launch(int num_warps) {
    extern int gpu_available;
    if (!gpu_available) {
        fprintf(stderr, "[GPU-EPCC] No GPU.\n");
        return -1;
    }

    extern uint32_t *g_gpu_epcc_lock_table;
    extern uint64_t *g_gpu_epcc_clock;
    extern uint32_t *g_gpu_epcc_data;
    extern int       g_gpu_epcc_num_addrs;
    extern uint64_t *g_gpu_epcc_committed;
    extern uint64_t *g_gpu_epcc_aborted;

    int reads_per_thread = 4;
    int writes_per_thread = 2;

    size_t shared_bytes = (2 * GPU_EPCC_MAX_READS * 32 + GPU_EPCC_MAX_WRITES * 32 + 1) * sizeof(uint32_t);

    gpu_epcc_kernel<GPU_EPCC_MAX_READS, GPU_EPCC_MAX_WRITES>
        <<<num_warps, GPU_EPCC_WARP_SIZE, shared_bytes>>>(
            g_gpu_epcc_lock_table,
            g_gpu_epcc_clock,
            g_gpu_epcc_data,
            g_gpu_epcc_num_addrs,
            g_gpu_epcc_committed,
            g_gpu_epcc_aborted,
            reads_per_thread,
            writes_per_thread
        );

    CUDA_CHECK(cudaDeviceSynchronize());

    uint64_t h_comm = 0, h_abort = 0;
    CUDA_CHECK(cudaMemcpy(&h_comm, g_gpu_epcc_committed, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&h_abort, g_gpu_epcc_aborted, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));

    printf("[GPU-EPCC] Kernel done: %lu commits, %lu aborts\n", h_comm, h_abort);
    return (int)h_comm;
}
