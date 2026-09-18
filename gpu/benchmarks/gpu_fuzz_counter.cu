// ── GPU batch fuzz counter benchmark ─────────────────────────────
//
// Demonstrates batch execution of TM transactions on the GPU via
// the CSMV batch executor.  Each transaction owns its OWN counter cell
// (indexed by warp_id) and increments it `increments` times.  The batch
// is therefore conflict-free: every transaction commits and the sum of
// all cells is exactly num_txns * increments, which is checked at the end.
//
// (A single shared cell would make almost every concurrent transaction in
// the batch abort, so a `counter == num_txns*increments` check could never
// hold.  See review-03.)
//
// Usage:
//   ./gpu_fuzz_counter [transactions] [increments_per_tx]

#include <cstdio>
#include <cstdlib>
#include "tm_gpu_platform.hpp"

#include "csmv_api.h"
#include "csmv_batch_executor.hpp"

#define CSMV_FUZZ_MAX_TX 4096

// ── Device-side counters (one cell per transaction) ─────────────
__device__ uint64_t g_counters[CSMV_FUZZ_MAX_TX];

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

    // Each transaction owns one cell → no cross-transaction conflict.
    uint64_t *cell = &g_counters[warp_id % CSMV_FUZZ_MAX_TX];

    csmv_gpu_begin(ws);
    for (int i = 0; i < tx_arg->increments; i++) {
        uint64_t val = csmv_gpu_read(ws, cell);   // newest version ≤ start_clock
        val++;
        csmv_gpu_write(ws, cell, val);
    }
    csmv_gpu_commit(ws);
}

// Device-side function-pointer slot.  The batch kernel makes an indirect
// device call, so enqueue() must receive a *device* function pointer, not the
// host stub (see csmv_batch_executor.hpp).  Initialize it device-side and
// read the value back on the host via cudaMemcpyFromSymbol (same idiom as
// gpu_ycsb.cu).  See review-03.
__device__ csmv_tx_body_t g_tx_fn = tx_increment;

// ── Snapshot: read newest committed value from the version list ──
// CSMV never writes committed values back to g_counters (writes prepend
// version nodes), so results must be read via the version-list head.
__device__ inline uint64_t fuzz_current(uint64_t *rec) {
    uint64_t idx = csmv_gpu_entry_idx(rec);
    CSMVGpuEntry *entry = &csmv_gpu_table()[idx];
    CSMVVersionNode *head = csmv_gpu_load_head(entry);
    return head ? head->value : 0;
}
__global__ void fuzz_snapshot_kernel(uint64_t *out, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = fuzz_current(&g_counters[i]);
}

// ── Host benchmark runner ───────────────────────────────────────

int main(int argc, char **argv) {
    int num_txns        = (argc > 1) ? atoi(argv[1]) : 256;
    int increments_per  = (argc > 2) ? atoi(argv[2]) : 8;
    if (num_txns < 1) num_txns = 1;
    if (num_txns > CSMV_FUZZ_MAX_TX) num_txns = CSMV_FUZZ_MAX_TX;

    printf("CSMV GPU Batch Fuzz Counter\n");
    printf("  Transactions: %d\n", num_txns);
    printf("  Increments per TX: %d\n", increments_per);
    printf("  Total operations: %d\n", num_txns * increments_per);
    printf("\n");

    // ── Initialize CSMV device state ───────────────────────────
    // Allocate the device-side version-list table before any transaction
    // runs; without this csmv_gpu_read/write dereference a null table and the
    // result is garbage.  Passing 0 uses the built-in default table size.
    // (This init call was previously left out as a TODO stub.  See review-03.)
    csmv_gpu_init(0);
    CSMVBatchExecutor executor;

    // Zero the counters.  &g_counters in host code is NOT a valid device
    // pointer for cudaMemset, so copy zeros via the symbol handle.
    static uint64_t zeros[CSMV_FUZZ_MAX_TX];  // zero-initialized
    cudaMemcpyToSymbol(g_counters, zeros, sizeof(zeros));

    // ── Enqueue transactions ────────────────────────────────────
    TxArg arg;
    arg.increments = increments_per;

    // Resolve the device function pointer once, then enqueue N txns (see
    // g_tx_fn above).
    csmv_tx_body_t d_fn;
    cudaMemcpyFromSymbol(&d_fn, g_tx_fn, sizeof(csmv_tx_body_t));

    for (int i = 0; i < num_txns; i++) {
        executor.enqueue(d_fn, &arg, sizeof(TxArg));
    }

    // ── Launch batch and measure ───────────────────────────────
    auto timing = executor.launch();
    executor.synchronize();

    // ── Read results back from device via the version list ─────
    uint64_t *d_out = nullptr;
    cudaMalloc(&d_out, num_txns * sizeof(uint64_t));
    fuzz_snapshot_kernel<<<(num_txns + 255) / 256, 256>>>(d_out, num_txns);
    cudaDeviceSynchronize();
    static uint64_t out[CSMV_FUZZ_MAX_TX];
    cudaMemcpy(out, d_out, num_txns * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree(d_out);
    uint64_t total = 0;
    for (int i = 0; i < num_txns; i++) total += out[i];

    printf("═══ Batch Results ═══\n");
    printf("  Transactions:     %d\n", num_txns);
    printf("  Expected total:   %d\n", num_txns * increments_per);
    printf("  Actual total:     %lu\n", total);
    printf("  %s\n", total == (uint64_t)(num_txns * increments_per)
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

    return (total == (uint64_t)(num_txns * increments_per)) ? 0 : 1;
}
