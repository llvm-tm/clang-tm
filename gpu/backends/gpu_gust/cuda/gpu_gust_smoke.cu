// ── GUST backend host harness / smoke test ─────────────────────
//
// Self-contained driver for the GUST batch executor (no TM-hooks
// infrastructure): allocates device memory, seeds VBoxes, enqueues a
// small batch of transfer transactions (one per lane), launches, then
// snapshots and verifies money conservation.
//
// Also exercises the raw microbenchmark launch path (gpu_gust_launch)
// when built with gpu_gust_runtime.cu + gpu_gust_host.cpp; the batch
// path below is self-contained and is the primary verification.
//
// Usage:
//   ./gpu_gust_smoke [warps] [accounts] [init_balance]
//
// Invariants verified:
//   1. commits + aborts == warps * 32      (every lane finalizes)
//   2. sum(balances) == accounts * init   (money conserved)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "gpu_gust_batch_executor.cuh"

// ── Transfer transaction: read src+dst, move `amount`. ─────────
struct TransferArg {
    int      num_accounts;
    int      init_balance;
    uint32_t seed;
};

__device__ inline uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

__device__ void tx_transfer(int lane_id, int warp_id,
                            void *arg, GUSTWarpState *ws)
{
    TransferArg *a = (TransferArg*)arg;
    uint32_t s = a->seed ^ (uint32_t)((warp_id * 32 + lane_id) * 2654435761u);
    uint32_t r = xorshift32(s);

    uint32_t src = r % (uint32_t)a->num_accounts;
    r = xorshift32(s);
    uint32_t dst = r % (uint32_t)a->num_accounts;
    if (dst == src) dst = (dst + 1) % (uint32_t)a->num_accounts;

    uint64_t sbal = gust_gpu_read(ws, src);
    uint64_t dbal = gust_gpu_read(ws, dst);

    r = xorshift32(s);
    uint32_t amount = (r % 100) + 1;
    if (amount > sbal) amount = (uint32_t)sbal;

    gust_gpu_write(ws, src, (uint32_t)(sbal - amount));
    gust_gpu_write(ws, dst, (uint32_t)(dbal + amount));

    gust_gpu_commit(ws);
}

__device__ gust_tx_body_t g_tx_fn = tx_transfer;

int main(int argc, char **argv) {
    int num_warps    = (argc > 1) ? atoi(argv[1]) : 4;
    int accounts     = (argc > 2) ? atoi(argv[2]) : 256;
    int init_balance = (argc > 3) ? atoi(argv[3]) : 1000;
    uint32_t seed    = (argc > 4) ? (uint32_t)strtoul(argv[4], nullptr, 10)
                                  : 20260731u;

    int num_txns = num_warps * 32;
    printf("GUST smoke test\n");
    printf("  Warps: %d, Transactions: %d, Accounts: %d, Init: %d\n",
           num_warps, num_txns, accounts, init_balance);

    // ── Init + seed ──────────────────────────────────────────
    gust_gpu_init(accounts);
    uint32_t *h_seed = new uint32_t[accounts];
    for (int i = 0; i < accounts; i++) h_seed[i] = (uint32_t)init_balance;
    gust_gpu_seed(h_seed, accounts);
    delete[] h_seed;

    // ── Enqueue + launch ─────────────────────────────────────
    GUSTBatchExecutor executor;
    gust_tx_body_t d_fn;
    cudaMemcpyFromSymbol(&d_fn, g_tx_fn, sizeof(gust_tx_body_t));

    for (int i = 0; i < num_txns; i++) {
        TransferArg a;
        a.num_accounts  = accounts;
        a.init_balance  = init_balance;
        a.seed          = seed + (uint32_t)i;
        executor.enqueue(d_fn, &a, sizeof(TransferArg));
    }

    auto timing = executor.launch();
    executor.synchronize();

    uint64_t commits = gust_gpu_committed_count();
    uint64_t aborts  = gust_gpu_aborted_count();

    // ── Verify ───────────────────────────────────────────────
    uint32_t *h_bal = new uint32_t[accounts];
    gust_gpu_snapshot(h_bal, accounts);
    uint64_t total = 0;
    for (int i = 0; i < accounts; i++) total += h_bal[i];
    uint64_t expected = (uint64_t)accounts * (uint64_t)init_balance;

    bool p1 = (commits + aborts == (uint64_t)num_txns);
    bool p2 = (total == expected);

    printf("═══ Results ═══\n");
    printf("  Commits: %llu, Aborts: %llu (total txns %d)\n",
           (unsigned long long)commits, (unsigned long long)aborts, num_txns);
    printf("  Balance sum: %lu (expected %lu)\n",
           (unsigned long)total, (unsigned long)expected);
    printf("  Kernel: %.3f ms, Throughput: %.1f txns/sec\n",
           timing.kernel_ms, num_txns / (timing.kernel_ms / 1000.0));
    printf("  Invariant 1 (all finalized): %s\n", p1 ? "PASS" : "FAIL");
    printf("  Invariant 2 (money conserved): %s\n", p2 ? "PASS" : "FAIL");
    printf("  %s\n", (p1 && p2) ? "PASS" : "FAIL");

    delete[] h_bal;
    gust_gpu_shutdown();
    return (p1 && p2) ? 0 : 1;
}
