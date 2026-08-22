// ── GPU Bank TM benchmark (GUST backend) ───────────────────────
//
// Models the STAMP "bank" workload on the GUST MVCC GPU backend via
// the GUST batch executor (one transaction per lane, warp-cooperative
// commit).  Each account maps to one VBox; a transfer transaction
// reads source + destination balances and writes both back, so the
// total money is conserved exactly.  A configurable fraction of
// transactions is read-only (the paper's RO-ratio sweep, RQ1), and the
// grid size is expressed in warps (scalability sweep, RQ2).
//
// Invariant:  sum of all committed account balances == num_accounts *
// init_balance.  Transfers preserve the sum; aborted transactions
// write nothing back, so the invariant holds regardless of aborts.
//
// Usage:
//   ./gpu_bank [num_warps] [accounts] [init_balance] [ro_ratio_percent]
//              [iterations]
//
// Defaults: 32 warps, 1024 accounts, 1000, 0% RO, 1 iteration.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "gpu_gust_batch_executor.cuh"

#define BANK_MAX_ACCOUNTS (1 << 20)

// ── Per-transaction argument ──────────────────────────────────
struct BankTxArg {
    int      num_accounts;
    int      init_balance;
    int      ro_ratio;      // % chance this tx is read-only
    uint32_t seed;          // per-tx RNG seed
};

__device__ inline uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// ── Transaction body: one transfer or one read-only scan per lane ─
__device__ void tx_bank(int lane_id, int warp_id,
                        void *arg, GUSTWarpState *ws)
{
    BankTxArg *a = (BankTxArg*)arg;

    // Per-lane RNG (each lane is its own transaction).
    uint32_t s = a->seed ^ (uint32_t)((warp_id * 32 + lane_id) * 2654435761u);
    uint32_t r = xorshift32(s);

    if ((int)(r % 100) < a->ro_ratio) {
        // Read-only: sum 4 accounts.  RO transactions never write, so
        // they only validate (CCT + MRV) and always commit unless a
        // concurrent writer published a newer version.
        for (int i = 0; i < 4; i++) {
            r = xorshift32(s);
            uint32_t acc = r % (uint32_t)a->num_accounts;
            gust_gpu_read(ws, acc);
        }
    } else {
        // Transfer: read src + dst, move `amount`.
        r = xorshift32(s);
        uint32_t src = r % (uint32_t)a->num_accounts;
        r = xorshift32(s);
        uint32_t dst = r % (uint32_t)a->num_accounts;
        if (dst == src) dst = (dst + 1) % (uint32_t)a->num_accounts;

        uint64_t sbal = gust_gpu_read(ws, src);
        uint64_t dbal = gust_gpu_read(ws, dst);

        r = xorshift32(s);
        uint32_t amount = (r % 100) + 1;
        if (amount > sbal) amount = (uint32_t)sbal;   // no negative balances

        gust_gpu_write(ws, src, (uint32_t)(sbal - amount));
        gust_gpu_write(ws, dst, (uint32_t)(dbal + amount));
    }

    gust_gpu_commit(ws);
}

__device__ gust_tx_body_t g_tx_fn = tx_bank;

// ── Host driver ───────────────────────────────────────────────
int main(int argc, char **argv) {
    int num_warps   = (argc > 1) ? atoi(argv[1]) : 32;
    int accounts    = (argc > 2) ? atoi(argv[2]) : 1024;
    int init_balance= (argc > 3) ? atoi(argv[3]) : 1000;
    int ro_ratio    = (argc > 4) ? atoi(argv[4]) : 0;
    int iterations  = (argc > 5) ? atoi(argv[5]) : 1;
    uint32_t seed   = (argc > 6) ? (uint32_t)strtoul(argv[6], nullptr, 10)
                                 : 20260731u;

    if (accounts > BANK_MAX_ACCOUNTS) {
        fprintf(stderr, "accounts must be <= %d\n", BANK_MAX_ACCOUNTS);
        return 1;
    }
    if (num_warps <= 0) {
        fprintf(stderr, "num_warps must be > 0\n");
        return 1;
    }

    int num_txns = num_warps * 32;      // one transaction per lane

    printf("GPU Bank TM benchmark (GUST)\n");
    printf("  Warps:        %d\n", num_warps);
    printf("  Transactions: %d (1/lane)\n", num_txns);
    printf("  Accounts:     %d\n", accounts);
    printf("  Init balance: %d\n", init_balance);
    printf("  RO ratio:     %d%%\n", ro_ratio);
    printf("  Iterations:   %d\n", iterations);
    printf("\n");

    // ── Initialize GUST device state ─────────────────────────
    gust_gpu_init(accounts);

    GUSTBatchExecutor executor;
    gust_tx_body_t d_fn;
    cudaMemcpyFromSymbol(&d_fn, g_tx_fn, sizeof(gust_tx_body_t));

    // Seed every VBox with the initial balance (version 1, head=1), so
    // snapshot reads at any startTS ≥ 1 see the initial value.
    {
        uint32_t *h_seed = new uint32_t[accounts];
        for (int i = 0; i < accounts; i++) h_seed[i] = (uint32_t)init_balance;
        gust_gpu_seed(h_seed, accounts);
        delete[] h_seed;
    }

    printf("═══ Batch Results ═══\n");
    bool pass = true;

    for (int it = 0; it < iterations; it++) {
        for (int i = 0; i < num_txns; i++) {
            BankTxArg a;
            a.num_accounts  = accounts;
            a.init_balance  = init_balance;
            a.ro_ratio      = ro_ratio;
            a.seed          = seed + (uint32_t)(it * num_txns + i);
            executor.enqueue(d_fn, &a, sizeof(BankTxArg));
        }

        auto timing = executor.launch();
        executor.synchronize();

        uint64_t commits = gust_gpu_committed_count();
        uint64_t aborts  = gust_gpu_aborted_count();

        // ── Snapshot and verify money conservation ────────────
        uint32_t *h_bal = new uint32_t[accounts];
        gust_gpu_snapshot(h_bal, accounts);
        uint64_t total = 0;
        for (int i = 0; i < accounts; i++) total += h_bal[i];
        uint64_t expected = (uint64_t)accounts * (uint64_t)init_balance;

        bool iter_pass = (total == expected);
        pass = pass && iter_pass;

        printf("  Iteration %d: commits=%llu aborts=%llu total=%lu "
               "expected=%lu %s\n",
               it, (unsigned long long)commits, (unsigned long long)aborts,
               (unsigned long)total, (unsigned long)expected,
               iter_pass ? "PASS" : "FAIL");

        if (it == iterations - 1) {
            printf("  Kernel time:  %.3f ms\n", timing.kernel_ms);
            printf("  Throughput:   %.1f txns/sec\n",
                   num_txns / (timing.kernel_ms / 1000.0));
        }
        delete[] h_bal;
    }
    printf("\n");
    printf("  Overall: %s\n", pass ? "PASS" : "FAIL");
    printf("\n");

    executor.synchronize();
    gust_gpu_shutdown();
    return pass ? 0 : 1;
}
