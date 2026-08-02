// GPU GUST kernel launch wrapper
#include "tm_gpu_platform.hpp"
#include "gpu_gust_api.h"
#include "gpu_gust_kernel.cuh"

int gpu_gust_launch(int num_warps) {
    extern int g_gpu_available;
    if (!g_gpu_available) {
        fprintf(stderr, "[GPU-GUST] No GPU.\n");
        return -1;
    }

    extern GUSTVBox    *g_gpu_gust_vboxes;
    extern uint64_t    *g_gpu_gust_gts;
    extern uint64_t    *g_gpu_gust_write_ptr;
    extern GUSTCLEntry *g_gpu_gust_cl;
    extern int          g_gpu_gust_num_addrs;
    extern uint64_t    *g_gpu_gust_committed;
    extern uint64_t    *g_gpu_gust_aborted;

    int reads_per_thread = 4;
    int writes_per_thread = 2;

    // Shared memory: read_addrs + read_vers + write_addrs + write_vals
    size_t shared_bytes = (32 * GPU_GUST_MAX_READS * 4
                           + 32 * GPU_GUST_MAX_READS * 8
                           + 32 * GPU_GUST_MAX_WRITES * 4
                           + 32 * GPU_GUST_MAX_WRITES * 4);

    gpu_gust_kernel<GPU_GUST_MAX_READS, GPU_GUST_MAX_WRITES>
        <<<num_warps, GPU_GUST_WARP_SIZE, shared_bytes>>>(
            g_gpu_gust_vboxes,
            g_gpu_gust_gts,
            g_gpu_gust_write_ptr,
            g_gpu_gust_cl,
            g_gpu_gust_num_addrs,
            g_gpu_gust_committed,
            g_gpu_gust_aborted,
            reads_per_thread,
            writes_per_thread
        );

    CUDA_CHECK(cudaDeviceSynchronize());

    uint64_t h_comm = 0, h_abort = 0;
    CUDA_CHECK(cudaMemcpy(&h_comm, g_gpu_gust_committed, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&h_abort, g_gpu_gust_aborted, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));

    printf("[GPU-GUST] Kernel done: %lu commits, %lu aborts\n", h_comm, h_abort);
    return (int)h_comm;
}
