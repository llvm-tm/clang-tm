// ── GPU Memcached TM benchmark (DRAFT) ─────────────────────────
//
// MemcachedGPU-style key-value workload (Kotni et al., ICPP 2016):
// a hash-table backed KV store where each transaction is a batch of
// GET / SET operations.  Adapted to the CSMV batch executor; each
// transaction is a single GET or SET (extension: multi-op txns via the
// write_ratio loop, same as gpu_ycsb).
//
// SIMD model: one warp per transaction.  All lanes derive the same key
// from the warp seed (uniform control flow), then the warp cooperatively
// walks the version list for that key's hash bucket.
//
// Invariant: every committed SET stores a value derived from its own key,
// so the final table contents can be re-derived and compared.  A SET of
// key K writes exactly K+1; a GET reads it back.  The snapshot kernel
// reads each key's newest committed value and the host verifies
// consistency (each stored value == its key+1).
//
// NOTE: draft skeleton — hash collision handling and multi-op txns are
// simplified.  See TODO markers.
//
// Usage:
//   ./gpu_memcached [keys] [txns] [write_ratio_percent]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "csmv_api.h"
#include "csmv_batch_executor.hpp"

#define GPU_MEMCACHED_MAX_KEYS (1 << 20)

// Logical KV table: one version-list entry per key (addressed by key).
__device__ uint64_t g_values[GPU_MEMCACHED_MAX_KEYS];

__device__ unsigned long long g_commits = 0;
__device__ unsigned long long g_aborts = 0;
__device__ unsigned long long g_gets = 0;
__device__ unsigned long long g_sets = 0;

struct MemcachedTxArg {
    int    keys;
    int    write_ratio;   // % chance this tx is a SET
    uint32_t seed;
};

__device__ inline uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// ── Transaction body: one GET or SET per warp ─────────────────
__device__ void tx_memcached(int lane_id, int warp_id,
                             void *arg, void *shared_scratch)
{
    CSMVWarpState *ws = (CSMVWarpState*)shared_scratch;
    MemcachedTxArg *a = (MemcachedTxArg*)arg;

    uint32_t s = a->seed ^ (uint32_t)(warp_id * 2654435761u);
    uint32_t r = xorshift32(s);
    uint32_t key = r % (uint32_t)a->keys;   // same on all lanes

    csmv_gpu_begin(ws);

    uint64_t cur = csmv_gpu_read(ws, &g_values[key]);

    if ((int)(r % 100) < a->write_ratio) {
        // SET key = key+1 (deterministic payload)
        csmv_gpu_write(ws, &g_values[key], (uint64_t)key + 1);
        if (csmv_gpu_commit(ws) != 0) {
            if (lane_id == 0) {
                atomicAdd((unsigned long long*)&g_commits, 1ULL);
                atomicAdd((unsigned long long*)&g_sets, 1ULL);
            }
        } else {
            if (lane_id == 0) atomicAdd((unsigned long long*)&g_aborts, 1ULL);
        }
    } else {
        // GET: no writes, read-only commit (never aborts in CSMV).
        if (csmv_gpu_commit(ws) != 0) {
            if (lane_id == 0) {
                atomicAdd((unsigned long long*)&g_commits, 1ULL);
                atomicAdd((unsigned long long*)&g_gets, 1ULL);
            }
        } else {
            if (lane_id == 0) atomicAdd((unsigned long long*)&g_aborts, 1ULL);
        }
    }
}

// ── Snapshot: newest committed value per key ─────────────────
__device__ inline uint64_t current_value(uint64_t *kv) {
    uint64_t idx = csmv_gpu_entry_idx(kv);
    CSMVGpuEntry *entry = &g_csmv_gpu_head_table[idx];
    CSMVVersionNode *head = csmv_gpu_load_head(entry);
    return head ? head->value : 0;
}

__global__ void snapshot_kernel(uint64_t *out, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = current_value(&g_values[i]);
}

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

    printf("GPU Memcached TM benchmark (CSMV, draft)\n");
    printf("  Keys:         %d\n", keys);
    printf("  Transactions: %d\n", num_txns);
    printf("  Write ratio:  %d%%\n", write_ratio);
    printf("\n");

    cudaMemset(g_values, 0, sizeof(g_values));
    csmv_gpu_init(GPU_MEMCACHED_MAX_KEYS);

    CSMVBatchExecutor executor;
    for (int i = 0; i < num_txns; i++) {
        MemcachedTxArg a;
        a.keys        = keys;
        a.write_ratio = write_ratio;
        a.seed        = seed + (uint32_t)i;
        executor.enqueue(tx_memcached, &a, sizeof(MemcachedTxArg));
    }

    auto timing = executor.launch();
    executor.synchronize();

    // Snapshot
    uint64_t *h_out = new uint64_t[keys];
    uint64_t *d_out;
    cudaMalloc(&d_out, keys * sizeof(uint64_t));
    snapshot_kernel<<<(keys + 255) / 256, 256>>>(d_out, keys);
    cudaDeviceSynchronize();
    cudaMemcpy(h_out, d_out, keys * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree(d_out);

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
    csmv_gpu_shutdown();
    return pass ? 0 : 1;
}
