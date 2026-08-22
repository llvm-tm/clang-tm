// ── GPU Memcached TM benchmark (GUST backend) ─────────────────
//
// MemcachedGPU-style key-value workload (Kotni et al., ICPP 2016):
// a hash-table backed KV store where each transaction is a GET or a
// SET.  Adapted to the GUST batch executor (one transaction per lane,
// warp-cooperative commit).  Each key maps to one VBox.
//
// Invariant: every committed SET stores value key+1 (deterministic
// payload), so the final table contents can be re-derived.  The
// snapshot kernel reads each key's newest committed value and the host
// verifies consistency (each non-zero stored value == its key+1).
//
// Usage:
//   ./gpu_memcached_gust [keys] [txns] [write_ratio_percent] [seed]
//
// txns must be a multiple of 32 (one transaction per lane).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "gpu_gust_batch_executor.cuh"

#define GPU_MEMCACHED_MAX_KEYS (1 << 20)

__device__ unsigned long long g_commits = 0;
__device__ unsigned long long g_aborts = 0;
__device__ unsigned long long g_gets = 0;
__device__ unsigned long long g_sets = 0;

struct MemcachedTxArg {
    int      keys;
    int      write_ratio;   // % chance this tx is a SET
    uint32_t seed;
};

__device__ inline uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// ── Transaction body: one GET or one SET per lane ─────────────
__device__ void tx_memcached(int lane_id, int warp_id,
                             void *arg, GUSTWarpState *ws)
{
    MemcachedTxArg *a = (MemcachedTxArg*)arg;
    uint32_t s = a->seed ^ (uint32_t)((warp_id * 32 + lane_id) * 2654435761u);
    uint32_t r = xorshift32(s);
    uint32_t key = r % (uint32_t)a->keys;

    uint64_t cur = gust_gpu_read(ws, key);

    if ((int)(r % 100) < a->write_ratio) {
        // SET key = key+1 (deterministic payload)
        gust_gpu_write(ws, key, key + 1);
        if (gust_gpu_commit(ws) != 0) {
            atomicAdd((unsigned long long*)&g_commits, 1ULL);
            atomicAdd((unsigned long long*)&g_sets, 1ULL);
        } else {
            atomicAdd((unsigned long long*)&g_aborts, 1ULL);
        }
    } else {
        // GET: read-only commit (validation only).
        if (gust_gpu_commit(ws) != 0) {
            atomicAdd((unsigned long long*)&g_commits, 1ULL);
            atomicAdd((unsigned long long*)&g_gets, 1ULL);
        } else {
            atomicAdd((unsigned long long*)&g_aborts, 1ULL);
        }
    }
}

__device__ gust_tx_body_t g_tx_fn = tx_memcached;

// ── Host driver ───────────────────────────────────────────────
int main(int argc, char **argv) {
    int keys        = (argc > 1) ? atoi(argv[1]) : 4096;
    int num_txns    = (argc > 2) ? atoi(argv[2]) : 1024;
    int write_ratio = (argc > 3) ? atoi(argv[3]) : 50;
    uint32_t seed   = (argc > 4) ? (uint32_t)strtoul(argv[4], nullptr, 10)
                                 : 20260731u;

    if (keys > GPU_MEMCACHED_MAX_KEYS) {
        fprintf(stderr, "keys must be <= %d\n", GPU_MEMCACHED_MAX_KEYS);
        return 1;
    }
    if (num_txns % 32 != 0) {
        fprintf(stderr, "txns must be a multiple of 32 (one tx per lane)\n");
        return 1;
    }

    printf("GPU Memcached TM benchmark (GUST)\n");
    printf("  Keys:         %d\n", keys);
    printf("  Transactions: %d\n", num_txns);
    printf("  Write ratio:  %d%%\n", write_ratio);
    printf("\n");

    gust_gpu_init(keys);

    GUSTBatchExecutor executor;
    gust_tx_body_t d_fn;
    cudaMemcpyFromSymbol(&d_fn, g_tx_fn, sizeof(gust_tx_body_t));

    // Zero-init all VBoxes.
    uint32_t *h_zero = new uint32_t[keys];
    for (int i = 0; i < keys; i++) h_zero[i] = 0;
    gust_gpu_seed(h_zero, keys);
    delete[] h_zero;

    for (int i = 0; i < num_txns; i++) {
        MemcachedTxArg a;
        a.keys        = keys;
        a.write_ratio = write_ratio;
        a.seed        = seed + (uint32_t)i;
        executor.enqueue(d_fn, &a, sizeof(MemcachedTxArg));
    }

    auto timing = executor.launch();
    executor.synchronize();

    // Snapshot
    uint32_t *h_out = new uint32_t[keys];
    gust_gpu_snapshot(h_out, keys);

    unsigned long long commits, aborts, gets, sets;
    cudaMemcpyFromSymbol(&commits, g_commits, sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&aborts,  g_aborts,  sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&gets,    g_gets,    sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&sets,    g_sets,    sizeof(unsigned long long));

    // ── Verify: every non-zero stored value == its key+1 ────
    uint64_t mismatch = 0;
    for (int i = 0; i < keys; i++) {
        if (h_out[i] != 0 && h_out[i] != (uint64_t)i + 1) mismatch++;
    }
    bool pass = (commits + aborts == (unsigned long long)num_txns) &&
                (gets + sets == commits) && (mismatch == 0);

    printf("═══ Batch Results ═══\n");
    printf("  Commits: %llu (GET %llu, SET %llu)\n", commits, gets, sets);
    printf("  Aborts:  %llu\n", aborts);
    printf("  Value mismatches: %lu\n", mismatch);
    printf("  %s\n", pass ? "PASS" : "FAIL");
    printf("\n");
    printf("═══ GPU Timing ═══\n");
    printf("  Kernel: %.3f ms\n", timing.kernel_ms);
    printf("  Total:  %.3f ms\n", timing.total_ms);
    printf("  Tx/sec: %.1f\n", num_txns / (timing.total_ms / 1000.0));
    printf("\n");

    delete[] h_out;
    gust_gpu_shutdown();
    return pass ? 0 : 1;
}
