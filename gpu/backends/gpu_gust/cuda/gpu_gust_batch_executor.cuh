#pragma once

#include <cstdint>
#include <vector>

#include "tm_gpu_platform.hpp"
#include "gpu_gust_api.h"
#include "gpu_gust_kernel.cuh"

// ── GUST GPU Batch Executor ─────────────────────────────────────
//
// Runs real benchmark transactions (YCSB-style, Memcached, Bank, …)
// on the GUST MVCC backend.  The batch executor mirrors the CSMV batch
// executor's shape, but adapted to GUST's transaction model: ONE
// transaction PER LANE, warp-cooperative commit (AtomicINC CL insertion
// + hybrid CCT/MRV validation + batch publication).
//
// Model:
//   - The host enqueues N transaction bodies (`gust_tx_body_t`) with
//     per-transaction argument structs (deep-copied at enqueue time,
//     like CSMVBatchExecutor).
//   - `gpu_gust_batch_kernel` launches one warp per 32 transactions
//     (N must be a multiple of 32).  Each lane owns a GUSTWarpState in
//     shared memory, snapshots GTS, calls the body, then the body
//     invokes the warp-cooperative `gust_gpu_commit` itself (CSMV-style:
//     the body checks the commit result to bump per-benchmark counters).
//   - Commit reuses the verified protocol from gpu_gust_kernel.cuh:
//     intra-warp pre-validation, CL insertion, CCT + MRV validation,
//     version append, warp-batch GTS publication.
//
// Transaction-body interface:
//   void body(int lane_id, int warp_id, void *arg, GUSTWarpState *ws)
//   - call gust_gpu_begin(ws)          (snapshot; done by the kernel)
//   - gust_gpu_read(ws, vbox_idx)      → newest committed value ≤ startTS
//   - gust_gpu_write(ws, vbox_idx, v)  (buffered; dedupes same address)
//   - ALL lanes must reach gust_gpu_commit(ws) together (uniform flow).
//     Returns nonzero commit timestamp on commit, 0 on abort.

// ── Per-lane transaction state (shared memory) ─────────────────
struct GUSTWarpState {
    uint64_t startTS;
    int      num_reads;
    int      num_writes;
    uint32_t read_addrs[GPU_GUST_MAX_READS];   // vbox indices read
    uint32_t write_addrs[GPU_GUST_MAX_WRITES]; // vbox indices written
    uint32_t write_vals[GPU_GUST_MAX_WRITES];  // buffered values
};

using gust_tx_body_t = void (*)(int lane_id, int warp_id,
                                void *arg, GUSTWarpState *ws);

#if defined(__CUDACC__) || defined(__HIPCC__)

// ── Device pointer accessors (defined in gpu_gust_batch_executor.cu,
//    stored as __device__ globals; function decls avoid the nvcc
//    header-definition trap that hits extern __device__ globals). ──
__device__ GUSTVBox*    gust_gpu_vboxes();
__device__ uint64_t*    gust_gpu_gts();
__device__ uint64_t*    gust_gpu_write_ptr();
__device__ GUSTCLEntry* gust_gpu_cl();
__device__ uint64_t*    gust_gpu_committed();
__device__ uint64_t*    gust_gpu_aborted();

// ── Snapshot / begin ───────────────────────────────────────────
__device__ inline uint64_t gust_gpu_begin(GUSTWarpState *ws) {
    ws->startTS    = atomicAdd((unsigned long long*)gust_gpu_gts(), 0ULL);
    ws->num_reads  = 0;
    ws->num_writes = 0;
    return ws->startTS;
}

// ── Read: newest committed value ≤ startTS, recording the read-set.
//    Checks the write-set first so own writes are visible. ──────
__device__ inline uint64_t gust_gpu_read(GUSTWarpState *ws, uint32_t vbox_idx) {
    for (int i = 0; i < ws->num_writes; i++) {
        if (ws->write_addrs[i] == vbox_idx) return ws->write_vals[i];
    }
    GUSTVBox *vb = &gust_gpu_vboxes()[vbox_idx];
    uint64_t ver;
    uint32_t val;
    gpu_gust_vbox_read_value(vb, ws->startTS, &ver, &val);
    if (ws->num_reads < GPU_GUST_MAX_READS) {
        ws->read_addrs[ws->num_reads] = vbox_idx;
        ws->num_reads++;
    }
    return val;
}

// ── Write: buffer into the private write-set (last write wins). ─
__device__ inline void gust_gpu_write(GUSTWarpState *ws, uint32_t vbox_idx,
                                      uint32_t val) {
    for (int i = 0; i < ws->num_writes; i++) {
        if (ws->write_addrs[i] == vbox_idx) { ws->write_vals[i] = val; return; }
    }
    if (ws->num_writes < GPU_GUST_MAX_WRITES) {
        ws->write_addrs[ws->num_writes] = vbox_idx;
        ws->write_vals[ws->num_writes] = val;
        ws->num_writes++;
    }
}

// ── Warp-cooperative commit.  ALL 32 lanes call this in lockstep
//    (the tx body must have uniform control flow up to this point).
//    Mirrors gpu_gust_kernel.cuh's Phases 3–6 + batch publication,
//    operating on this lane's GUSTWarpState.  Returns CTS+1 on
//    commit, 0 on abort. ─────────────────────────────────────────
__device__ inline uint64_t gust_gpu_commit(GUSTWarpState *ws) {
    const int lane = threadIdx.x & 31;
    const int my_reads   = ws->num_reads;
    const int my_writes  = ws->num_writes;
    const uint32_t *my_ra = ws->read_addrs;
    const uint32_t *my_wa = ws->write_addrs;

    // ── Phase 3: PRE-VALIDATION (intra-warp conflicts) ─────────
    // Write counts differ per lane (read-only txns have 0), but the
    // __ballot_sync inside the loop requires every lane to reach it the
    // SAME number of times.  Reduce the warp max first (portable shfl,
    // all lanes participate), then iterate the fixed bound with a
    // per-lane guard so all lanes execute identical ballot counts.
    int max_writes = my_writes;
    #pragma unroll
    for (int off = 16; off > 0; off >>= 1) {
        int t = __shfl_down_sync(~0ULL, max_writes, off);
        if (t > max_writes) max_writes = t;
    }
    int conflict = 0;
    for (int w = 0; w < max_writes; w++) {
        uint32_t a = (w < my_writes) ? my_wa[w] : (uint32_t)-1;
        uint64_t rmask = __ballot_sync(~0ULL,
                          (w < my_writes) &&
                          gpu_gust_read_contains(a, my_ra, my_reads));
        uint64_t wmask = __ballot_sync(~0ULL,
                          (w < my_writes) &&
                          gpu_gust_write_contains(a, my_wa, my_writes));
        uint64_t lower = (1ull << lane) - 1;
        if (w < my_writes && ((rmask | wmask) & lower)) conflict = 1;
    }
    __syncwarp();

    // ── Phase 4: CL INSERTION (AtomicINC, warp-cooperative) ────
    uint64_t base = 0;
    if (lane == 0) {
        base = atomicAdd((unsigned long long*)gust_gpu_write_ptr(),
                         (unsigned long long)GPU_GUST_WARP_SIZE);
    }
    base = __shfl_sync(~0ULL, base, 0);
    const uint64_t CTS = base + (uint64_t)lane;

    uint32_t cl_slot = (uint32_t)(CTS & GPU_GUST_CL_MASK);
    GUSTCLEntry *my_entry = &gust_gpu_cl()[cl_slot];

    int is_aborted = conflict;
    if (my_entry->state != GPU_GUST_CL_FREE) is_aborted = 1;

    if (is_aborted) {
        my_entry->state = GPU_GUST_CL_ABORTED;
    } else {
        my_entry->state = GPU_GUST_CL_PENDING;
        my_entry->num_writes = (uint32_t)my_writes;
        for (int w = 0; w < my_writes; w++) {
            my_entry->write_addrs[w] = my_wa[w];
            my_entry->write_vals[w]  = ws->write_vals[w];
        }
    }
    __threadfence();

    // ── Phase 5: VALIDATION (hybrid CCT + MRV) ────────────────
    if (!is_aborted) {
        int64_t valPtr = (int64_t)CTS - 1;
        const int64_t start = (int64_t)ws->startTS;
        while (valPtr > start) {
            if ((uint64_t)valPtr < *gust_gpu_gts()) {
                for (int i = 0; i < my_reads; i++) {
                    if (gpu_gust_vbox_has_newer(&gust_gpu_vboxes()[my_ra[i]],
                                                ws->startTS)) {
                        is_aborted = 1;
                        break;
                    }
                }
                break;
            }
            GUSTCLEntry *e = &gust_gpu_cl()[(uint32_t)((uint64_t)valPtr
                                                       & GPU_GUST_CL_MASK)];
            if (e->state == GPU_GUST_CL_ABORTED) { valPtr--; continue; }
            if (e->state != GPU_GUST_CL_FREE) {
                int nw = (int)e->num_writes;
                for (int i = 0; i < my_reads && !is_aborted; i++) {
                    for (int j = 0; j < nw; j++) {
                        if (e->write_addrs[j] == my_ra[i]) { is_aborted = 1; break; }
                    }
                }
            }
            valPtr--;
        }
    }

    if (is_aborted) {
        my_entry->state = GPU_GUST_CL_ABORTED;
        __threadfence();
    } else {
        // ── Phase 6: WRITE-BACK ────────────────────────────────
        for (int w = 0; w < my_writes; w++) {
            GUSTVBox *vb = &gust_gpu_vboxes()[my_wa[w]];
            uint32_t slot = (uint32_t)(atomicAdd(&vb->head, 1u)
                                       & (GPU_GUST_VBOX_DEPTH - 1));
            vb->versions[slot] = CTS + 1;
            __threadfence();
            vb->values[slot] = ws->write_vals[w];
        }
        my_entry->state = GPU_GUST_CL_COMMITTED;
        __threadfence();
    }
    __syncwarp();

    // ── Batch publication ──────────────────────────────────────
    uint64_t cmask = __ballot_sync(~0ULL, is_aborted ? 0u : 1u);
    if (lane == 0) {
        while (*(volatile uint64_t*)gust_gpu_gts() < base) { }
        atomicAdd((unsigned long long*)gust_gpu_gts(),
                  (unsigned long long)GPU_GUST_WARP_SIZE);
        atomicAdd((unsigned long long*)gust_gpu_committed(),
                  (unsigned long long)__popc((unsigned int)cmask));
        atomicAdd((unsigned long long*)gust_gpu_aborted(),
                  (unsigned long long)(GPU_GUST_WARP_SIZE
                                       - __popc((unsigned int)cmask)));
    }
    return is_aborted ? 0 : CTS + 1;
}

// ── Batch kernel: one warp per 32 transactions, one tx per lane. ─
// `num_txns` MUST be a multiple of GPU_GUST_WARP_SIZE (the warp
// publication protocol advances GTS by a full warp; partial warps
// would leave the batch publication deadlock-prone).
__global__ void gpu_gust_batch_kernel(gust_tx_body_t fn, void **args,
                                      int num_txns);

// ── Snapshot kernel: newest committed value per vbox (for host
//    invariant verification after all batches complete). ─────────
__global__ void gust_gpu_snapshot_kernel(uint32_t *out, int n);

#endif // __CUDACC__ / __HIPCC__

// ── Host-side batch executor ───────────────────────────────────
struct GUSTBatchWorkItem {
    gust_tx_body_t  fn;       // device function pointer
    void           *arg;      // device-side argument pointer
    size_t          arg_size;
    std::vector<uint8_t> host_arg_copy;  // snapshot at enqueue() time
};

class GUSTBatchExecutor {
public:
    GUSTBatchExecutor();
    ~GUSTBatchExecutor();

    // Deep-copies `arg` immediately (safe for stack-locals reused across
    // enqueues).  All transactions must share one device function.
    void enqueue(gust_tx_body_t fn, void *arg, size_t arg_size);

    struct BatchTiming {
        float kernel_ms;
        float h2d_ms;
        float d2h_ms;
        float total_ms;
    };
    BatchTiming launch();

    void synchronize();
    void **get_device_args() { return d_args_; }
    int    get_batch_size()  { return (int)batch_.size(); }

    struct ProfileEvent {
        float kernel_ms; float h2d_ms; float d2h_ms; int tx_count;
    };
    const std::vector<ProfileEvent>& get_profile_events() const {
        return profile_events_;
    }

private:
    std::vector<GUSTBatchWorkItem> batch_;
    std::vector<ProfileEvent> profile_events_;

    void      **d_args_;
    char       *d_arg_data_;
    size_t      d_arg_capacity_;
    int         stream_idx_;
};

// ── Host-side device lifecycle (allocates vboxes[0..num_addrs), GTS,
//    write pointer, commit log, counters; stores pointers in __device__
//    globals).  Implemented in gpu_gust_batch_executor.cu. ─────────
extern "C" void gust_gpu_init(int num_addrs);
extern "C" void gust_gpu_shutdown(void);
extern "C" void gust_gpu_snapshot(uint32_t *h_out, int n);
extern "C" void gust_gpu_seed(const uint32_t *h_vals, int n);
extern "C" uint64_t gust_gpu_committed_count(void);
extern "C" uint64_t gust_gpu_aborted_count(void);
