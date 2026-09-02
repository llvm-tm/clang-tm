// ── GPU K-means TM benchmark (DRAFT) ───────────────────────────
//
// STAMP kmeans transactional kernel adapted to the GPU CSMV batch
// executor.  No LLVM plugin: device tx bodies call the CSMV API.
//
// Transactional work (mirrors kmeans_accumulate's tx_retry block):
//   read   points[i*D .. i*D+D-1]          (the point being assigned)
//   read   centroids[c*D .. c*D+D-1]       (all K centroids)
//   write  assignments[i]                  (nearest-cluster id)
//
// SIMD model: one warp per transaction (per point).  All 32 lanes walk
// the same centroid list in lockstep (warp-uniform control flow), so the
// distance computation is cooperative and divergence-free.
//
// NOTE: this is a draft skeleton — algorithm choices marked (TODO.md: GPU benchmark stubs, P1).
// are deliberately simplified for the batch-executor pattern.
//
// Reads of points/centroids for the *distance computation* use the raw
// device arrays directly (read-only data, initialized before launch);
// the CSMV calls form the read set for validation only.  The assignment
// write goes through the TM (version list), so it must be read back with
// a snapshot kernel (raw array is never updated by CSMV commits).
//
// Usage:
//   ./gpu_kmeans [npoints] [ndims] [nclusters] [txns_per_batch]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#include "csmv_api.h"
#include "csmv_batch_executor.hpp"

#define GPU_KMEANS_MAX_POINTS  (1 << 20)
#define GPU_KMEANS_MAX_DIMS    8
#define GPU_KMEANS_MAX_CLUST  64

__device__ double g_points[GPU_KMEANS_MAX_POINTS * GPU_KMEANS_MAX_DIMS];
__device__ double g_centroids[GPU_KMEANS_MAX_CLUST * GPU_KMEANS_MAX_DIMS];
__device__ uint64_t g_assignments[GPU_KMEANS_MAX_POINTS];  // 8B cells: matches
                                                           // CSMV entry_idx
                                                           // granularity (>>3)

__device__ unsigned long long g_commits = 0;
__device__ unsigned long long g_aborts = 0;


struct KMeansTxArg {
    int    nclusters;
    int    ndims;
    int    point_idx;    // which point this tx assigns
};

// ── Distance kernel fragment: all lanes evaluate the same centroid ──
// Each lane handles the dimensions it owns; warp-reduce via shfl.
__device__ inline double warp_dist_sq(const double *p, const double *c, int ndims) {
    int lane = threadIdx.x & 31;
    double local = 0.0;
    // Striped: lane l computes dims {l, l+32, ...} — SIMD, no divergence.
    for (int d = lane; d < ndims; d += 32) {
        double diff = p[d] - c[d];
        local += diff * diff;
    }
    // Warp reduction
    for (int off = 16; off > 0; off >>= 1) {
        local += __shfl_xor_sync(~0u, local, off);
    }
    return local;   // identical on all lanes
}

// ── Version-list head reader (matches ycsb/memcached snapshot pattern) ──
__device__ inline uint64_t current_value(uint64_t *cell) {
    uint64_t idx = csmv_gpu_entry_idx(cell);
    CSMVGpuEntry *entry = &csmv_gpu_table()[idx];
    CSMVVersionNode *head = csmv_gpu_load_head(entry);
    return head ? head->value : 0;
}

__global__ void snapshot_assignments(uint64_t *out, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = current_value(&g_assignments[i]);
}

__device__ void tx_kmeans_assign(int lane_id, int warp_id,
                                 void *arg, void *shared_scratch)
{
    CSMVWarpState *ws = (CSMVWarpState*)shared_scratch;
    KMeansTxArg *a = (KMeansTxArg*)arg;

    csmv_gpu_begin(ws);

    const double *p = &g_points[(size_t)a->point_idx * a->ndims];

    // All lanes compute the same point's best centroid (uniform loop).
    int    best = 0;
    double best_d = warp_dist_sq(p, &g_centroids[0], a->ndims);
    for (int c = 1; c < a->nclusters; c++) {
        double d = warp_dist_sq(p, &g_centroids[c * a->ndims], a->ndims);
        if (d < best_d) { best_d = d; best = c; }
    }
    // (Lane-uniform: best is identical on all 32 lanes.)

    // Read the point's own data through TM (forms the read set).
    double own[GPU_KMEANS_MAX_DIMS];
    for (int d = 0; d < a->ndims; d++) {
        own[d] = csmv_gpu_read(ws, &g_points[(size_t)a->point_idx * a->ndims + d]);
    }
    // Read centroids through TM (read set for validation).
    for (int c = 0; c < a->nclusters; c++) {
        for (int d = 0; d < a->ndims; d++) {
            csmv_gpu_read(ws, &g_centroids[c * a->ndims + d]);
        }
    }

    // Write the assignment through TM (single-word write).
    csmv_gpu_write(ws, &g_assignments[a->point_idx], (uint64_t)(uint32_t)best);

    if (csmv_gpu_commit(ws) != 0) {
        if (lane_id == 0) atomicAdd((unsigned long long*)&g_commits, 1ULL);
    } else {
        if (lane_id == 0) atomicAdd((unsigned long long*)&g_aborts, 1ULL);
    }
}

__device__ csmv_tx_body_t g_tx_fn = tx_kmeans_assign;

// ── Host driver ───────────────────────────────────────────────
int main(int argc, char **argv) {
    int npoints   = (argc > 1) ? atoi(argv[1]) : 4096;
    int ndims     = (argc > 2) ? atoi(argv[2]) : 2;
    int nclusters = (argc > 3) ? atoi(argv[3]) : 8;
    int txns      = (argc > 4) ? atoi(argv[4]) : 1024;

    if (npoints > GPU_KMEANS_MAX_POINTS || ndims > GPU_KMEANS_MAX_DIMS ||
        nclusters > GPU_KMEANS_MAX_CLUST) {
        fprintf(stderr, "limits exceeded (points<=%d dims<=%d clusters<=%d)\n",
                GPU_KMEANS_MAX_POINTS, GPU_KMEANS_MAX_DIMS, GPU_KMEANS_MAX_CLUST);
        return 1;
    }
    if (txns > npoints) txns = npoints;

    printf("GPU K-means TM benchmark (CSMV, draft)\n");
    printf("  Points:   %d\n", npoints);
    printf("  Dims:     %d\n", ndims);
    printf("  Clusters: %d\n", nclusters);
    printf("  Txns:     %d\n", txns);
    printf("\n");

    // ── Initialize data (host → device) ─────────────────────
    // g_assignments is NOT initialized here: reads go through the CSMV
    // version list (empty list => value 0), never the raw array.
    double *h_points = new double[npoints * ndims];
    double *h_cent   = new double[nclusters * ndims];
    for (int i = 0; i < npoints * ndims; i++) h_points[i] = (double)(i % 100) / 3.0;
    for (int i = 0; i < nclusters * ndims; i++) h_cent[i] = (double)(i % 10);
    cudaMemcpyToSymbol(g_points,     h_points, (size_t)npoints * ndims * sizeof(double));
    cudaMemcpyToSymbol(g_centroids,  h_cent,   (size_t)nclusters * ndims * sizeof(double));

    csmv_gpu_init(GPU_KMEANS_MAX_POINTS * GPU_KMEANS_MAX_DIMS);
    CSMVBatchExecutor executor;

    csmv_tx_body_t d_fn;
    cudaMemcpyFromSymbol(&d_fn, g_tx_fn, sizeof(csmv_tx_body_t));

    for (int i = 0; i < txns; i++) {
        KMeansTxArg a;
        a.nclusters = nclusters;
        a.ndims     = ndims;
        a.point_idx = i;   // each tx assigns a distinct point (SIMD: distinct data)
        executor.enqueue(d_fn, &a, sizeof(KMeansTxArg));
    }

    auto timing = executor.launch();
    executor.synchronize();

    unsigned long long commits, aborts;
    cudaMemcpyFromSymbol(&commits, g_commits, sizeof(unsigned long long));
    cudaMemcpyFromSymbol(&aborts,  g_aborts,  sizeof(unsigned long long));

    // ── Snapshot assignments (via version-list heads) and sanity-check ──
    uint64_t *h_result = new uint64_t[txns];
    uint64_t *d_out;
    cudaMalloc(&d_out, txns * sizeof(uint64_t));
    snapshot_assignments<<<(txns + 255) / 256, 256>>>(d_out, txns);
    cudaDeviceSynchronize();
    cudaMemcpy(h_result, d_out, txns * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree(d_out);

    int valid = 0;
    for (int i = 0; i < txns; i++) {
        int asgn = (int)h_result[i];   // 0..nclusters-1; uncommitted == 0
        if (asgn < 0 || asgn >= nclusters) continue;
        // recompute brute-force nearest on host
        int    best = 0;
        double bd = 1e30;
        for (int c = 0; c < nclusters; c++) {
            double d = 0;
            for (int dd = 0; dd < ndims; dd++) {
                double df = h_points[i*ndims+dd] - h_cent[c*ndims+dd];
                d += df*df;
            }
            if (d < bd) { bd = d; best = c; }
        }
        if (h_result[i] == best) valid++;
    }

    bool pass = (commits + aborts == (unsigned long long)txns) && (valid == txns);

    printf("═══ Batch Results ═══\n");
    printf("  Commits:     %llu\n", commits);
    printf("  Aborts:      %llu\n", aborts);
    printf("  Assign valid:%d/%d\n", valid, txns);
    printf("  %s\n", pass ? "PASS" : "FAIL");
    printf("\n");
    printf("═══ GPU Timing ═══\n");
    printf("  Kernel:  %.3f ms\n", timing.kernel_ms);
    printf("  Total:   %.3f ms\n", timing.total_ms);
    printf("  Tx/sec:  %.1f\n", txns / (timing.total_ms / 1000.0));
    printf("\n");

    delete[] h_points; delete[] h_cent; delete[] h_result;
    csmv_gpu_shutdown();
    return pass ? 0 : 1;
}
