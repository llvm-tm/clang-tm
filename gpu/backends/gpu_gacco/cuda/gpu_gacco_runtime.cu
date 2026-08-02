// GPU GAccO kernel launch wrapper
#include "tm_gpu_platform.hpp"
#include "gpu_gacco_api.h"
#include "gpu_gacco_kernel.cuh"

int gpu_gacco_launch(int num_warps) {
    extern int g_gpu_available;
    if (!g_gpu_available) {
        fprintf(stderr, "[GPU-GAccO] No GPU.\n");
        return -1;
    }

    extern uint32_t *g_gpu_gacco_lock_table;
    extern uint64_t *g_gpu_gacco_clock;
    extern uint32_t *g_gpu_gacco_data;
    extern int       g_gpu_gacco_num_addrs;
    extern uint64_t *g_gpu_gacco_committed;
    extern uint64_t *g_gpu_gacco_aborted;

    int reads_per_thread = 4;
    int writes_per_thread = 2;

    // Shared memory: read_addrs + read_vers + write_tmp + abort_flag
    size_t shared_bytes = (2 * GPU_GACCO_MAX_READS * 32 + GPU_GACCO_MAX_WRITES * 32 + 1) * sizeof(uint32_t);

    gpu_gacco_kernel<GPU_GACCO_MAX_READS, GPU_GACCO_MAX_WRITES>
        <<<num_warps, GPU_GACCO_WARP_SIZE, shared_bytes>>>(
            g_gpu_gacco_lock_table,
            g_gpu_gacco_clock,
            g_gpu_gacco_data,
            g_gpu_gacco_num_addrs,
            g_gpu_gacco_committed,
            g_gpu_gacco_aborted,
            reads_per_thread,
            writes_per_thread
        );

    CUDA_CHECK(cudaDeviceSynchronize());

    uint64_t h_comm = 0, h_abort = 0;
    CUDA_CHECK(cudaMemcpy(&h_comm, g_gpu_gacco_committed, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&h_abort, g_gpu_gacco_aborted, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));

    printf("[GPU-GAccO] Kernel done: %lu commits, %lu aborts\n", h_comm, h_abort);
    return (int)h_comm;
}
