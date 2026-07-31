// ── GPU batch fuzz counter benchmark ─────────────────────────────
//
// Demonstrates batch execution of TM transactions on the GPU via
// the CSMV batch executor.  Each transaction increments a shared
// counter N times (simulating fuzz_counter on GPU).
//
// Usage:
//   ./gpu_fuzz_counter [transactions] [increments_per_tx] [accounts]

#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>

#include "csmv_api.h"
#include "csmv_batch_executor.hpp"

// ── Global clock + counter on device ─────────────────────────────
__device__ uint64_t g_counter = 0;

// ── Per-transaction argument ─────────────────────────────────────
struct TxArg {
    int  increments;  // how many increments this tx does
    int  pad;
};

// ── Transaction body (runs on GPU) ──────────────────────────────
__device__ void tx_increment(int lane_id, int warp_id,
                              void *arg, void *shared_scratch)
{
    // The batch kernel passes this warp's own CSMVWarpState as
    // shared_scratch, so we use it directly.
    CSMVWarpState *ws = (CSMVWarpState*)shared_scratch;

    TxArg *tx_arg = (TxArg*)arg;

    csmv_gpu_begin(ws);

    // Read-modify-write the global counter
    for (int i = 0; i < tx_arg->increments; i++) {
        // Read via multi-version: find newest version ≤ start_clock
        uint64_t val = csmv_gpu_read(ws, &g_counter);
        val++;
        csmv_gpu_write(ws, &g_counter, val);
    }

    csmv_gpu_commit(ws);
}

// ── Host benchmark runner ───────────────────────────────────────

int main(int argc, char **argv) {
    int num_txns        = (argc > 1) ? atoi(argv[1]) : 256;
    int increments_per  = (argc > 2) ? atoi(argv[2]) : 8;

    printf("CSMV GPU Batch Fuzz Counter\n");
    printf("  Transactions: %d\n", num_txns);
    printf("  Increments per TX: %d\n", increments_per);
    printf("  Total operations: %d\n", num_txns * increments_per);
    printf("\n");

    // ── Initialize CSMV device state ───────────────────────────
    CSMVBatchExecutor executor;

    // Register the counter for TM tracking by initializing its version list
    // (in a real setup, csmv_init would do this)
    cudaMemset(&g_counter, 0, sizeof(g_counter));

    // ── Enqueue transactions ────────────────────────────────────
    TxArg arg;
    arg.increments = increments_per;

    for (int i = 0; i < num_txns; i++) {
        executor.enqueue(tx_increment, &arg, sizeof(TxArg));
    }

    // ── Launch batch and measure ───────────────────────────────
    auto timing = executor.launch();
    executor.synchronize();

    // ── Read result back from device ───────────────────────────
    uint64_t final_val;
    cudaMemcpyFromSymbol(&final_val, g_counter, sizeof(uint64_t));

    printf("═══ Batch Results ═══\n");
    printf("  Transactions:     %d\n", num_txns);
    printf("  Expected counter: %d\n", num_txns * increments_per);
    printf("  Actual counter:   %lu\n", final_val);
    printf("  %s\n", final_val == (uint64_t)(num_txns * increments_per)
                     ? "PASS" : "FAIL");
    printf("\n");
    printf("═══ GPU Timing ═══\n");
    printf("  H2D transfer:  %.3f ms\n", timing.h2d_ms);
    printf("  Kernel:        %.3f ms\n", timing.kernel_ms);
    printf("  D2H transfer:  %.3f ms\n", timing.d2h_ms);
    printf("  Total:         %.3f ms\n", timing.total_ms);
    printf("  Throughput:    %.1f txns/sec\n",
           num_txns / (timing.total_ms / 1000.0));
    printf("\n");

    // Print profile summary
    printf("═══ Batch Profile ═══\n");
    printf("  tx_count=%d kernel_ms=%.2f h2d_ms=%.2f d2h_ms=%.2f\n",
           num_txns, timing.kernel_ms, timing.h2d_ms, timing.d2h_ms);

    return (final_val == (uint64_t)(num_txns * increments_per)) ? 0 : 1;
}
