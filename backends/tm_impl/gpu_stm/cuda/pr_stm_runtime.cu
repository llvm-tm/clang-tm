// ── PR-STM CUDA/HIP Kernel Launch Wrapper ──────────────────────
// Just the kernel launch.  Host-only runtime lives in
// pr_stm_host.cpp so the device linker doesn't see host symbols.
//
// This file is compiled by hipcc/nvcc so <<<>>> syntax is available.

#include "tm_gpu_platform.hpp"
#include "gpu_stm_api.h"
#include "pr_stm_kernel.cuh"

// ── Persistent GPU kernel launch ───────────────────────────────────

int gpu_pr_stm_launch(int num_warps,
                       pr_stm_tx_body_t tx_body,
                       void *tx_data) {
    (void)num_warps; (void)tx_body; (void)tx_data;
    // Check GPU availability via host-side flag
    extern int gpu_available;
    if (!gpu_available) {
        fprintf(stderr, "[PR-STM] No GPU: cannot launch kernel.\n");
        return -1;
    }

    extern uint32_t *d_lock_table;
    extern uint64_t *d_global_clock;
    extern uint32_t *d_data;
    extern int       gpu_num_addresses;
    extern uint64_t *d_committed;
    extern uint64_t *d_aborted;

    int reads_per_thread = 4;
    int writes_per_thread = 2;

    pr_stm_kernel<PR_STM_MAX_READS, PR_STM_MAX_WRITES>
        <<<num_warps, PR_STM_WARP_SIZE>>>(
            d_lock_table,
            d_global_clock,
            d_data,
            gpu_num_addresses,
            d_committed,
            d_aborted,
            reads_per_thread,
            writes_per_thread
        );

    CUDA_CHECK(cudaDeviceSynchronize());

    uint64_t h_comm = 0, h_abort = 0;
    CUDA_CHECK(cudaMemcpy(&h_comm, d_committed, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&h_abort, d_aborted, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));

    printf("[PR-STM] Kernel complete: %lu commits, %lu aborts\n",
           h_comm, h_abort);
    return (int)h_comm;
}
