// ── GPU YCSB-style TM benchmark (GUST backend) ──────────────────
//
// YCSB read/write workload on a shared-memory table, running on the
// GUST MVCC GPU backend via the GUST batch executor (one transaction
// per lane, warp-cooperative commit).  Each record maps to one VBox.
//
// Workload:
//   - A table of RECORDS uint64_t cells, all initial value 0.
//   - Each transaction performs OPS operations; each op reads a record
//     and, with probability WRITE_RATIO, writes it back incremented by
//     1 (read-modify-write).
//   - One lane runs one transaction.
//
// Invariant:
//   final_sum == committed_writes
//   Each committed write op adds exactly +1 to one record, so the table
//   sum must equal the total number of committed write ops.  Aborted
//   transactions contribute nothing.
//
// Usage:
//   ./gpu_ycsb_gust [records] [txns] [ops] [write_ratio_percent] [seed]
//
// txns must be a multiple of 32 (one transaction per lane, GUST batch
// publication advances GTS by a full warp).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "gpu_gust_batch_executor.cuh"

#define YCSB_MAX_RECORDS (1 << 20)

// ── Commit / abort counters ────────────────────────────────────
__device__ unsigned long long g_commits = 0;
__device__ unsigned long long g_aborts = 0;
__device__ unsigned long long g_committed_writes = 0;

// ── Per-transaction arguments ──────────────────────────────────
struct YCSBTxArg {
    int      records;        // active record count (< YCSB_MAX_RECORDS)
    int      ops;            // operations per transaction
    int      write_ratio;    // percent chance an op is a write
    uint32_t seed;           // per-tx RNG seed
};

__device__ inline uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// ── Transaction body (one per lane) ────────────────────────────
__device__ void tx_ycsb(int lane_id, int warp_id,
                        void *arg, GUSTWarpState *ws)
{
    YCSBTxArg *a = (YCSBTxArg*)arg;
    uint32_t s = a->seed ^ (uint32_t)((warp_id * 32 + lane_id) * 2654435761u);

    int writes = 0;
    for (int i = 0; i < a->ops; i++) {
        uint32_t r = xorshift32(s);
        uint32_t idx = r % (uint32_t)a->records;

        uint64_t val = gust_gpu_read(ws, idx);

        if ((int)(r % 100) < a->write_ratio) {
            gust_gpu_write(ws, idx, (uint32_t)(val + 1));
            writes++;
        }
    }

    if (gust_gpu_commit(ws) != 0) {
        atomicAdd((unsigned long long*)&g_commits, 1ULL);
        atomicAdd((unsigned long long*)&g_committed_writes,
                  (unsigned long long)writes);
    } else {
        atomicAdd((unsigned long long*)&g_aborts, 1ULL);
    }
}

__device__ gust_tx_body_t g_tx_fn = tx_ycsb;

// ── Host driver ───────────────────────────────────────────────
int main(int argc, char **argv) {
    int records     = (argc > 1) ? atoi(argv[1]) : 4096;
    int num_txns    = (argc > 2) ? atoi(argv[2]) : 1024;
    int ops         = (argc > 3) ? atoi(argv[3]) : 8;
    int write_ratio = (argc > 4) ? atoi(argv[4]) : 50;
    uint32_t seed   = (argc > 5) ? (uint32_t)strtoul(argv[5], nullptr, 10)
                                 : 20260731u;

    if (records > YCSB_MAX_RECORDS) {
        fprintf(stderr, "records must be <= %d\n", YCSB_MAX_RECORDS);
        return 1;
    }
    if (num_txns % 32 != 0) {
        fprintf(stderr, "txns must be a multiple of 32 (one tx per lane)\n");
        return 1;
    }

    printf("GPU YCSB-style TM benchmark (GUST)\n");
    printf("  Records:      %d\n", records);
    printf("  Transactions: %d\n", num_txns);
    printf("  Ops per tx:   %d\n", ops);
    printf("  Write ratio:  %d%%\n", write_ratio);
    printf("\n");

    // ── Initialize GUST device state ─────────────────────────
    gust_gpu_init(records);

    GUSTBatchExecutor executor;
    gust_tx_body_t d_fn;
    cudaMemcpyFromSymbol(&d_fn, g_tx_fn, sizeof(gust_tx_body_t));

    // Zero-initialize all VBoxes (gust_gpu_init memsets them).
    uint32_t *h_zero = new uint32_t[records];
    for (int i = 0; i < records; i++) h_zero[i] = 0;
    gust_gpu_seed(h_zero, records);
    delete[] h_zero;

    // ── Enqueue transactions ──────────────────────────────────
    for (int i = 0; i < num_txns; i++) {
        YCSBTxArg a;
        a.records     = records;
        a.ops         = ops;
        a.write_ratio = write_ratio;
        a.seed        = seed + (uint32_t)i;
        executor.enqueue(d_fn, &a, sizeof(YCSBTxArg));
    }

    // ── Launch batch and measure ─────────────────────────────
    auto timing = executor.launch();
    executor.synchronize();

    // ── Snapshot final table state ───────────────────────────
    uint32_t *h_out = new uint32_t[records];
    gust_gpu_snapshot(h_out, records);

    unsigned long long commits, aborts, committed_writes;
    cudaMemcpyFromSymbol(&commits,          g_commits,          sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&aborts,           g_aborts,           sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&committed_writes, g_committed_writes, sizeof(unsigned long long));

    // ── Verify invariant ─────────────────────────────────────
    uint64_t final_sum = 0;
    for (int i = 0; i < records; i++) final_sum += h_out[i];

    bool pass = (final_sum == committed_writes) &&
                (commits + aborts == (unsigned long long)num_txns);

    printf("═══ Batch Results ═══\n");
    printf("  Commits:         %llu\n", commits);
    printf("  Aborts:          %llu\n", aborts);
    printf("  Committed writes:%llu\n", committed_writes);
    printf("  Final sum:       %lu\n", final_sum);
    printf("  %s\n", pass ? "PASS" : "FAIL");
    printf("\n");
    printf("═══ GPU Timing ═══\n");
    printf("  Kernel:      %.3f ms\n", timing.kernel_ms);
    printf("  Total:       %.3f ms\n", timing.total_ms);
    printf("  Throughput:  %.1f txns/sec\n",
           num_txns / (timing.total_ms / 1000.0));
    printf("\n");

    delete[] h_out;
    gust_gpu_shutdown();
    return pass ? 0 : 1;
}
