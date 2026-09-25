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
    int      overflow;                         // set-set/read-set exceeded:
                                               // forces abort (review-05:
                                               // oversized writes were
                                               // silently dropped before)
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
__device__ uint64_t*    gust_gpu_overflow();

// ── Snapshot / begin ───────────────────────────────────────────
__device__ inline uint64_t gust_gpu_begin(GUSTWarpState *ws) {
    ws->startTS    = atomicAdd((unsigned long long*)gust_gpu_gts(), 0ULL);
    ws->num_reads  = 0;
    ws->num_writes = 0;
    ws->overflow   = 0;
    return ws->startTS;
}

// ── Read: newest committed value ≤ startTS, recording the read-set.
//    Checks the write-set first so own writes are visible.  A read that
//    does not fit the read-set marks overflow (the transaction must
//    abort: an untracked read could never be validated). ──────────
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
    } else {
        ws->overflow = 1;
    }
    return val;
}

// ── Write: buffer into the private write-set (last write wins).  A
//    write that does not fit marks overflow instead of being silently
//    dropped (review-05: a dropped write is a lost update). ───────
__device__ inline void gust_gpu_write(GUSTWarpState *ws, uint32_t vbox_idx,
                                      uint32_t val) {
    for (int i = 0; i < ws->num_writes; i++) {
        if (ws->write_addrs[i] == vbox_idx) { ws->write_vals[i] = val; return; }
    }
    if (ws->num_writes < GPU_GUST_MAX_WRITES) {
        ws->write_addrs[ws->num_writes] = vbox_idx;
        ws->write_vals[ws->num_writes] = val;
        ws->num_writes++;
    } else {
        ws->overflow = 1;
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
    // SAME number of times.  Reduce the warp max first with a
    // __shfl_xor butterfly (review-05: a shfl_down + local max is NOT a
    // reduction — lanes ended with different maxima, so the loop ran a
    // different number of ballots per lane and the partial-mask
    // __ballot_sync raised a hardware exception on AMD), then iterate
    // the fixed bound with a per-lane guard so all lanes execute
    // identical ballot counts.
    int max_writes = my_writes;
    #pragma unroll
    for (int off = 16; off > 0; off >>= 1) {
        int t = __shfl_xor_sync(~0ULL, max_writes, off);
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

    // is_aborted folds in the set-overflow flag (review-05: oversized
    // write-sets were silently truncated, dropping buffered writes —
    // lost updates that survived as committed partial state).
    int is_aborted = conflict || ws->overflow;
    int owns_slot  = 1;
    uint64_t old_cts = *(volatile uint64_t*)&my_entry->cts;
    if (old_cts != 0 && old_cts != CTS) {
        if ((uint64_t)*(volatile uint64_t*)gust_gpu_gts() <= old_cts) {
            is_aborted = 1;
            owns_slot  = 0;   // never trample a live foreign entry
        }
    }

    if (owns_slot) {
        // Payload first, epoch tag next, state LAST as the publish
        // marker (review-05: old code published state=PENDING before the
        // write-set payload, so concurrent CCT scans could validate
        // against an empty/stale write-set).
        my_entry->num_writes = (uint32_t)(is_aborted ? 0 : my_writes);
        if (!is_aborted) {
            for (int w = 0; w < my_writes; w++) {
                my_entry->write_addrs[w] = my_wa[w];
                my_entry->write_vals[w]  = ws->write_vals[w];
            }
        }
        my_entry->cts = CTS;
        __threadfence();                     // release
        my_entry->state = is_aborted ? GPU_GUST_CL_ABORTED
                                     : GPU_GUST_CL_PENDING;
    }

    // ── Phase 5: VALIDATION (hybrid CCT + MRV) ────────────────
    if (!is_aborted) {
        int64_t valPtr = (int64_t)CTS - 1;
        const int64_t start = (int64_t)ws->startTS;
        while (valPtr > start) {
            if ((uint64_t)valPtr < *(volatile uint64_t*)gust_gpu_gts()) {
                break;   // finalized region covered by the MRV below
            }
            GUSTCLEntry *e = &gust_gpu_cl()[(uint32_t)((uint64_t)valPtr
                                                       & GPU_GUST_CL_MASK)];
            // The slot reservation (AtomicINC) outpaces entry
            // publication: spin until this epoch publishes (bounded —
            // the owner publishes independently of GTS).
            while (*(volatile uint64_t*)&e->cts != (uint64_t)valPtr &&
                   *(volatile uint64_t*)gust_gpu_gts() <= (uint64_t)valPtr) { }
            if (*(volatile uint64_t*)&e->cts != (uint64_t)valPtr) {
                valPtr--; continue;          // stale generation
            }
            __threadfence();                 // acquire
            uint32_t st = *(volatile uint32_t*)&e->state;
            if (st != GPU_GUST_CL_ABORTED) {
                int nw = (int)e->num_writes;
                for (int i = 0; i < my_reads && !is_aborted; i++) {
                    for (int j = 0; j < nw; j++) {
                        if (e->write_addrs[j] == my_ra[i]) { is_aborted = 1; break; }
                    }
                }
            }
            valPtr--;
        }
        // MRV — unconditional (spec Valid = CCT ∧ MRV).
        if (!is_aborted) {
            for (int i = 0; i < my_reads; i++) {
                if (gpu_gust_vbox_has_newer(&gust_gpu_vboxes()[my_ra[i]],
                                            ws->startTS)) {
                    is_aborted = 1;
                    break;
                }
            }
        }
    }

    if (is_aborted) {
        if (owns_slot) {
            __threadfence();
            my_entry->state = GPU_GUST_CL_ABORTED;
        }
    } else {
        // ── Phase 6: WRITE-BACK ────────────────────────────────
        // Reserve all slots first; VBox-window overflow aborts instead
        // of clobbering live versions (reserved-but-unfilled slots keep
        // version 0 = sentinel and are skipped by readers).  Publish
        // value first, version last (marker).
        uint32_t slot_of[GPU_GUST_MAX_WRITES];
        int overflow = 0;
        for (int w = 0; w < my_writes; w++) {
            GUSTVBox *vb = &gust_gpu_vboxes()[my_wa[w]];
            uint32_t s = (uint32_t)atomicAdd(&vb->head, 1u);
            slot_of[w] = s & (GPU_GUST_VBOX_DEPTH - 1);
            if (s >= GPU_GUST_VBOX_DEPTH) overflow = 1;
        }
        if (overflow) {
            is_aborted = 1;
            // Count window overflows separately: unlike validation
            // aborts these would never recover without between-launch
            // compaction (review-06).
            atomicAdd((unsigned long long*)gust_gpu_overflow(), 1ull);
            __threadfence();
            my_entry->state = GPU_GUST_CL_ABORTED;
        } else {
            for (int w = 0; w < my_writes; w++) {
                GUSTVBox *vb = &gust_gpu_vboxes()[my_wa[w]];
                vb->values[slot_of[w]] = ws->write_vals[w];  // payload first
                __threadfence();
                vb->versions[slot_of[w]] = CTS + 1;          // marker last
            }
            __threadfence();
            my_entry->state = GPU_GUST_CL_COMMITTED;
        }
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
// VBox-window overflow aborts in the latest batch (0 = healthy).
extern "C" uint64_t gust_gpu_overflow_count(void);
// Compact every VBox window to its newest GPU_GUST_VBOX_KEEP versions
// and reset head.  Only call while NO kernel is in flight (quiescent
// point): correctness relies on every committed version being ≤ GTS,
// which holds strictly between launches of the batch executor.
extern "C" void gust_gpu_vbox_compact(void);
