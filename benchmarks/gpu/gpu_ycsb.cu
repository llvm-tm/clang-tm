// ── GPU YCSB-style TM benchmark ────────────────────────────────
//
// Models a YCSB read/write workload on a shared-memory table using
// the CSMV multi-version GPU TM backend via the batch executor.
// No LLVM plugin: transaction bodies are device functions that call
// csmv_gpu_begin/read/write/commit directly.
//
// Workload:
//   - A table of RECORDS uint64_t cells, all initial value 0.
//   - Each transaction performs OPS total operations; each operation
//     reads a record, and with probability WRITE_RATIO also writes it
//     back incremented by 1 (read-modify-write).
//   - One warp cooperatively executes one transaction.
//
// Invariant:
//   final_sum == committed_writes
//   Each committed write op adds exactly +1 to exactly one record (own-writes
//   are read back from the write set), so the table sum must equal the total
//   number of committed write ops.  Aborted transactions contribute nothing.
//
// Usage:
//   ./gpu_ycsb [records] [txns] [ops] [write_ratio_percent] [seed]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "csmv_api.h"
#include "csmv_batch_executor.hpp"

// ── Table on device (fixed-size global, like gpu_fuzz_counter) ──
#define YCSB_MAX_RECORDS (1 << 20)
__device__ uint64_t g_table[YCSB_MAX_RECORDS];

// ── Commit / abort counters ────────────────────────────────────
__device__ unsigned long long g_commits = 0;
__device__ unsigned long long g_aborts = 0;
__device__ unsigned long long g_committed_writes = 0;

// ── Per-transaction arguments ──────────────────────────────────
struct YCSBTxArg {
    int    records;          // active record count (< YCSB_MAX_RECORDS)
    int    ops;              // operations per transaction
    int    write_ratio;      // percent chance an op is a write
    uint32_t seed;           // per-tx RNG seed
};

// ── xorshift32 (identical across all lanes of a warp) ─────────
__device__ inline uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// ── Transaction body (runs on GPU, one warp per tx) ───────────
__device__ void tx_ycsb(int lane_id, int warp_id,
                        void *arg, void *shared_scratch)
{
    CSMVWarpState *ws = (CSMVWarpState*)shared_scratch;
    YCSBTxArg *a = (YCSBTxArg*)arg;

    csmv_gpu_begin(ws);

    // All lanes derive the same record/op sequence from the warp seed,
    // so the warp traverses each version list in lockstep.
    uint32_t s = a->seed ^ (uint32_t)(warp_id * 2654435761u);

    int writes = 0;
    for (int i = 0; i < a->ops; i++) {
        uint32_t r = xorshift32(s);
        uint32_t idx = r % (uint32_t)a->records;
        uint64_t *rec = &g_table[idx];

        uint64_t val = csmv_gpu_read(ws, rec);

        if ((int)(r % 100) < a->write_ratio) {
            csmv_gpu_write(ws, rec, val + 1);
            writes++;
        }
    }

    if (csmv_gpu_commit(ws) != 0) {
        if (lane_id == 0) {
            atomicAdd((unsigned long long*)&g_commits, 1ULL);
            atomicAdd((unsigned long long*)&g_committed_writes,
                      (unsigned long long)writes);
        }
    } else {
        if (lane_id == 0) atomicAdd((unsigned long long*)&g_aborts, 1ULL);
    }
}

// ── Snapshot kernel: read newest committed value of each record ──
// Reads the version-list head directly (writes prepend, so head is newest).
__device__ inline uint64_t current_value(uint64_t *rec) {
    uint64_t idx = csmv_gpu_entry_idx(rec);
    CSMVGpuEntry *entry = &g_csmv_gpu_head_table[idx];
    CSMVVersionNode *head = csmv_gpu_load_head(entry);
    return head ? head->value : 0;
}

__global__ void snapshot_kernel(uint64_t *out, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = current_value(&g_table[i]);
}

// ── Host driver ───────────────────────────────────────────────
int main(int argc, char **argv) {
    int records    = (argc > 1) ? atoi(argv[1]) : 4096;
    int num_txns   = (argc > 2) ? atoi(argv[2]) : 1024;
    int ops        = (argc > 3) ? atoi(argv[3]) : 8;
    int write_ratio= (argc > 4) ? atoi(argv[4]) : 50;
    uint32_t seed  = (argc > 5) ? (uint32_t)strtoul(argv[5], nullptr, 10)
                                : 20260731u;

    if (records > YCSB_MAX_RECORDS) {
        fprintf(stderr, "records must be <= %d\n", YCSB_MAX_RECORDS);
        return 1;
    }

    printf("GPU YCSB-style TM benchmark (CSMV)\n");
    printf("  Records:      %d\n", records);
    printf("  Transactions: %d\n", num_txns);
    printf("  Ops per tx:   %d\n", ops);
    printf("  Write ratio:  %d%%\n", write_ratio);
    printf("\n");

    cudaMemset(g_table, 0, sizeof(g_table));

    // ── Initialize CSMV device state ──────────────────────────
    csmv_gpu_init(YCSB_MAX_RECORDS);

    CSMVBatchExecutor executor;

    // ── Enqueue transactions ──────────────────────────────────
    for (int i = 0; i < num_txns; i++) {
        YCSBTxArg a;
        a.records     = records;
        a.ops         = ops;
        a.write_ratio = write_ratio;
        a.seed        = seed + (uint32_t)i;
        executor.enqueue(tx_ycsb, &a, sizeof(YCSBTxArg));
    }

    // ── Launch batch and measure ─────────────────────────────
    auto timing = executor.launch();
    executor.synchronize();

    // ── Snapshot final table state ───────────────────────────
    uint64_t *h_out = new uint64_t[records];
    uint64_t *d_out;
    cudaMalloc(&d_out, records * sizeof(uint64_t));
    snapshot_kernel<<<(records + 255) / 256, 256>>>(d_out, records);
    cudaDeviceSynchronize();
    cudaMemcpy(h_out, d_out, records * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree(d_out);

    unsigned long long commits, aborts, committed_writes;
    cudaMemcpyFromSymbol(&commits,          g_commits,          sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&aborts,           g_aborts,           sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&committed_writes, g_committed_writes, sizeof(unsigned long long));

    // ── Verify invariant ─────────────────────────────────────
    uint64_t final_sum = 0;
    for (int i = 0; i < records; i++) final_sum += h_out[i];

    // Each committed write op adds exactly +1 to exactly one record,
    // so the final table sum must equal the total committed-write count.
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
    printf("  H2D transfer: %.3f ms\n", timing.h2d_ms);
    printf("  Kernel:       %.3f ms\n", timing.kernel_ms);
    printf("  Total:        %.3f ms\n", timing.total_ms);
    printf("  Throughput:   %.1f txns/sec\n",
           num_txns / (timing.total_ms / 1000.0));
    printf("\n");

    delete[] h_out;
    csmv_gpu_shutdown();
    return pass ? 0 : 1;
}
