// ── GPU TPC-C TM benchmark (DRAFT) ─────────────────────────────
//
// TPC-C Payment transaction adapted to the CSMV batch executor.
// Payment is the classic high-contention TPC-C transaction:
//
//   read   WAREHOUSE(w_id)               → w_ytd, w_tax
//   read   DISTRICT(d_id, w_id)          → d_ytd, d_tax
//   read   CUSTOMER(c_id, d_id, w_id)    → c_balance
//   write  CUSTOMER.balance   -= amount
//   write  CUSTOMER.ytd       += amount
//   write  WAREHOUSE.ytd      += amount
//   write  DISTRICT.ytd       += amount
//   write  HISTORY             += record (new row)
//
// Invariant: each committed payment applies four deltas
//   w_ytd += amt, d_ytd += amt, c_balance -= amt, c_ytd += amt
// whose net effect on the total is exactly +2*amt.  So the running sum
// of all four arrays must equal 2*amt*commits.  The snapshot kernel
// sums the four arrays and the host checks against the committed count
// (if any of the four writes failed to apply atomically, the sum would
// not match).
//
// SIMD model: one warp per transaction.  All lanes derive the same
// (w, d, c) keys from the warp seed, then cooperatively read/write the
// four table cells.
//
// NOTE: draft skeleton — the HISTORY append and the c_data 200-char
// field are elided for the word-granularity version list (see TODO).
//
// Usage:
//   ./gpu_tpcc [warehouses] [districts] [customers] [txns] [amount]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "csmv_api.h"
#include "csmv_batch_executor.hpp"

#define GPU_TPCC_MAX_W 64
#define GPU_TPCC_MAX_D 10
#define GPU_TPCC_MAX_C 3000

// Table layout (flattened, one uint64 per field — version-list granularity):
//   WYT[nw]  w_ytd (stored as cents to stay integer)
//   DYT[nw][nd]  d_ytd
//   CBAL[nw][nd][nc]  c_balance
//   CYT[nw][nd][nc]   c_ytd_payment
// Sum of all four arrays grows by exactly 2*amount per committed payment
// (see invariant above); the host checks total == 2*amount*commits.

#define WYT_IDX(w)       (w)
#define DYT_IDX(w,d)     ((w) * GPU_TPCC_MAX_D + (d))
#define CBAL_IDX(w,d,c)  (((w) * GPU_TPCC_MAX_D + (d)) * GPU_TPCC_MAX_C + (c))
#define CYT_IDX(w,d,c)   (CBAL_IDX(w,d,c) + GPU_TPCC_MAX_W*GPU_TPCC_MAX_D*GPU_TPCC_MAX_C)

__device__ uint64_t g_wyt [GPU_TPCC_MAX_W];
__device__ uint64_t g_dyt [GPU_TPCC_MAX_W * GPU_TPCC_MAX_D];
__device__ uint64_t g_cbal[GPU_TPCC_MAX_W * GPU_TPCC_MAX_D * GPU_TPCC_MAX_C];
__device__ uint64_t g_cyt [GPU_TPCC_MAX_W * GPU_TPCC_MAX_D * GPU_TPCC_MAX_C];

__device__ unsigned long long g_commits = 0;
__device__ unsigned long long g_aborts = 0;


struct TPCCTxArg {
    int    nw, nd, nc;
    uint64_t amount;
    uint32_t seed;
};

__device__ inline uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// ── Payment transaction body ──────────────────────────────────
__device__ void tx_tpcc_payment(int lane_id, int warp_id,
                                void *arg, void *shared_scratch)
{
    CSMVWarpState *ws = (CSMVWarpState*)shared_scratch;
    TPCCTxArg *a = (TPCCTxArg*)arg;

    // All lanes derive the same (w, d, c).
    uint32_t s = a->seed ^ (uint32_t)(warp_id * 2654435761u);
    uint32_t w = xorshift32(s) % (uint32_t)a->nw;
    uint32_t d = xorshift32(s) % (uint32_t)a->nd;
    uint32_t c = xorshift32(s) % (uint32_t)a->nc;

    csmv_gpu_begin(ws);

    // Read phase (read set = 4 cells).
    uint64_t wyt  = csmv_gpu_read(ws, &g_wyt[WYT_IDX(w)]);
    uint64_t dyt  = csmv_gpu_read(ws, &g_dyt[DYT_IDX(w,d)]);
    uint64_t cbal = csmv_gpu_read(ws, &g_cbal[CBAL_IDX(w,d,c)]);
    uint64_t cyt  = csmv_gpu_read(ws, &g_cyt[CYT_IDX(w,d,c)]);

    // Money moves: customer pays amount.
    //   w_ytd += amt, d_ytd += amt, c_balance -= amt, c_ytd += amt
    csmv_gpu_write(ws, &g_wyt[WYT_IDX(w)],      wyt  + a->amount);
    csmv_gpu_write(ws, &g_dyt[DYT_IDX(w,d)],    dyt  + a->amount);
    csmv_gpu_write(ws, &g_cbal[CBAL_IDX(w,d,c)],cbal - a->amount);
    csmv_gpu_write(ws, &g_cyt[CYT_IDX(w,d,c)],  cyt  + a->amount);

    if (csmv_gpu_commit(ws) != 0) {
        if (lane_id == 0) atomicAdd((unsigned long long*)&g_commits, 1ULL);
    } else {
        if (lane_id == 0) atomicAdd((unsigned long long*)&g_aborts, 1ULL);
    }
}

__device__ csmv_tx_body_t g_tx_fn = tx_tpcc_payment;

// ── Version-list head readers ─────────────────────────────────
__device__ inline uint64_t head_value(uint64_t *cell) {
    uint64_t idx = csmv_gpu_entry_idx(cell);
    CSMVGpuEntry *entry = &csmv_gpu_table()[idx];
    CSMVVersionNode *head = csmv_gpu_load_head(entry);
    return head ? head->value : 0;
}
__device__ inline uint64_t current_wyt(int i)  { return head_value(&g_wyt[i]); }
__device__ inline uint64_t current_dyt(int i)  { return head_value(&g_dyt[i]); }
__device__ inline uint64_t current_cbal(int i) { return head_value(&g_cbal[i]); }
__device__ inline uint64_t current_cyt(int i)  { return head_value(&g_cyt[i]); }

// ── Snapshot kernel: sum all four arrays (money conservation) ──
__global__ void snapshot_kernel(uint64_t *out_wyt, uint64_t *out_dyt,
                                uint64_t *out_cbal, uint64_t *out_cyt,
                                int nw, int nd, int nc)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < nw)            out_wyt[i]  = current_wyt(WYT_IDX(i));
    if (i < nw * nd)       out_dyt[i]  = current_dyt(DYT_IDX(i / nd, i % nd));
    if (i < nw * nd * nc)  out_cbal[i] = current_cbal(CBAL_IDX(i / (nd*nc), (i / nc) % nd, i % nc));
    if (i < nw * nd * nc)  out_cyt[i]  = current_cyt(CYT_IDX(i / (nd*nc), (i / nc) % nd, i % nc));
}

// ── Host driver ───────────────────────────────────────────────
int main(int argc, char **argv) {
    int nw        = (argc > 1) ? atoi(argv[1]) : 8;
    int nd        = (argc > 2) ? atoi(argv[2]) : 4;
    int nc        = (argc > 3) ? atoi(argv[3]) : 64;
    int num_txns  = (argc > 4) ? atoi(argv[4]) : 1024;
    uint64_t amt  = (argc > 5) ? strtoull(argv[5], nullptr, 10) : 10;
    uint32_t seed = (argc > 6) ? (uint32_t)strtoul(argv[6], nullptr, 10)
                               : 20260731u;

    if (nw > GPU_TPCC_MAX_W || nd > GPU_TPCC_MAX_D || nc > GPU_TPCC_MAX_C) {
        fprintf(stderr, "limits exceeded\n");
        return 1;
    }

    size_t n_w  = nw;
    size_t n_d  = (size_t)nw * nd;
    size_t n_c  = (size_t)nw * nd * nc;
    size_t n_cyt = n_c;

    printf("GPU TPC-C Payment TM benchmark (CSMV, draft)\n");
    printf("  Warehouses:  %d\n", nw);
    printf("  Districts:   %d\n", nd);
    printf("  Customers:   %d\n", nc);
    printf("  Transactions:%d\n", num_txns);
    printf("  Amount:      %lu\n", amt);
    printf("\n");

    // Initial state: every cell 0 except c_balance = 1000 (arbitrary).
    // (Version list starts empty → reads return 0; we seed balance by
    //  pre-committing a value?  No: CSMV reads the version list only.
    //  For the money-conservation check we need an initial total; here we
    //  track the *sum of deltas* instead, which is exact.)

    csmv_gpu_init(GPU_TPCC_MAX_W * GPU_TPCC_MAX_D * GPU_TPCC_MAX_C * 2);

    CSMVBatchExecutor executor;

    csmv_tx_body_t d_fn;
    cudaMemcpyFromSymbol(&d_fn, g_tx_fn, sizeof(csmv_tx_body_t));

    for (int i = 0; i < num_txns; i++) {
        TPCCTxArg a;
        a.nw = nw; a.nd = nd; a.nc = nc;
        a.amount = amt;
        a.seed   = seed + (uint32_t)i;
        executor.enqueue(d_fn, &a, sizeof(TPCCTxArg));
    }

    auto timing = executor.launch();
    executor.synchronize();

    // Snapshot and sum
    size_t total_cells = n_w + n_d + n_c + n_cyt;
    uint64_t *h = new uint64_t[total_cells];
    uint64_t *d_out;
    cudaMalloc(&d_out, total_cells * sizeof(uint64_t));
    snapshot_kernel<<<(total_cells + 255) / 256, 256>>>(
        d_out, d_out + n_w, d_out + n_w + n_d, d_out + n_w + n_d + n_c,
        nw, nd, nc);
    cudaDeviceSynchronize();
    cudaMemcpy(h, d_out, total_cells * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree(d_out);

    unsigned long long commits, aborts;
    cudaMemcpyFromSymbol(&commits, g_commits, sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&aborts,  g_aborts,  sizeof(unsigned long long));

    // Invariant: every committed payment moves the sum of all fields by
    // exactly +2*amount (w_ytd +amt, d_ytd +amt, c_balance -amt, c_ytd +amt).
    // Aborted transactions contribute nothing (version nodes only on commit).
    uint64_t total = 0;
    for (size_t i = 0; i < total_cells; i++) total += h[i];

    uint64_t expected = 2ULL * amt * (uint64_t)commits;
    bool pass = (commits + aborts == (unsigned long long)num_txns) &&
                (total == expected);

    printf("═══ Batch Results ═══\n");
    printf("  Commits: %llu\n", commits);
    printf("  Aborts:  %llu\n", aborts);
    printf("  Sum of all fields: %lu (expected 2*amount*commits = %lu)\n",
           total, expected);
    printf("  %s\n", pass ? "PASS" : "FAIL");
    printf("\n");
    printf("═══ GPU Timing ═══\n");
    printf("  Kernel: %.3f ms\n", timing.kernel_ms);
    printf("  Total:  %.3f ms\n", timing.total_ms);
    printf("  Tx/sec: %.1f\n", num_txns / (timing.total_ms / 1000.0));
    printf("\n");

    delete[] h;
    csmv_gpu_shutdown();
    return pass ? 0 : 1;
}
