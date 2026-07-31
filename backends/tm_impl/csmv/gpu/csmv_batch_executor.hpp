#pragma once

#include <cstdint>
#include "tm_gpu_platform.hpp"
#include <vector>
#include <functional>

#include "../include/csmv_api.h"

// ── CSMV GPU Batch Executor ──────────────────────────────────────
//
// Queues transaction bodies and launches them as GPU kernels.
// Each warp processes one transaction, with warp-cooperative
// version list traversal for reads and warp-leader node creation
// for writes.
//
// Batching model:
//   1. User calls csmv_enqueue_batch(name, fn, arg) N times
//   2. User calls csmv_launch_batch() which:
//      a. Copies argument data GPU → H2D (async stream)
//      b. Launches kernel: gridDim = ceil(N / warps_per_block)
//      c. Each warp runs one transaction
//      d. Reads results back GPU → D2H (async stream)
//      e. Synchronizes
//   3. User calls csmv_get_batch_results() to read outputs

// ── Configuration ────────────────────────────────────────────────

#define CSMV_BATCH_MAX_TRANSACTIONS  1024
#define CSMV_BATCH_WARPS_PER_BLOCK   4
#define CSMV_BATCH_STREAM_COUNT      4

// ── Types ────────────────────────────────────────────────────────

using csmv_tx_body_t = void (*)(int lane_id, int warp_id,
                                void *arg, void *shared_scratch);

struct CSMVBatchWorkItem {
    csmv_tx_body_t  fn;       // device function pointer
    void           *arg;      // device-side argument pointer
    size_t          arg_size; // size of argument in bytes
    // Deep copy of the host argument captured at enqueue() time.  The caller
    // may pass a stack-local struct (its address is reused across loop
    // iterations), so the value must be snapshotted NOW, not deferred until
    // launch() (which would give every tx the last iteration's value).
    std::vector<uint8_t> host_arg_copy;
};

// ── Host-side batch executor ────────────────────────────────────

class CSMVBatchExecutor {
public:
    CSMVBatchExecutor();
    ~CSMVBatchExecutor();

    // Enqueue a transaction.  `fn` must be a *device* function pointer
    // (obtained via cudaMemcpyFromSymbol of a __device__ csmv_tx_body_t
    // variable, NOT the host stub of a __device__ function).  `arg` is
    // deep-copied immediately (so stack-locals reused across enqueues are
    // safe) and later copied to device-side storage.  All enqueued
    // transactions must use the same device function (stored-procedure model).
    void enqueue(csmv_tx_body_t fn, void *arg, size_t arg_size);

    // Launch all queued transactions as a single kernel.
    // Returns (kernel_time_ms, total_time_ms)
    struct BatchTiming {
        float kernel_ms;
        float h2d_ms;
        float d2h_ms;
        float total_ms;
    };
    BatchTiming launch();

    // Wait for completion and get results.
    // After this, enqueued work items are cleared.
    void synchronize();

    // Access device pointers for result reading.
    void **get_device_args() { return d_args_; }
    int    get_batch_size()  { return batch_.size(); }

    // Profiling
    struct ProfileEvent {
        float kernel_ms;
        float h2d_ms;
        float d2h_ms;
        int   tx_count;
    };
    const std::vector<ProfileEvent>& get_profile_events() const {
        return profile_events_;
    }

private:
    std::vector<CSMVBatchWorkItem> batch_;
    std::vector<ProfileEvent> profile_events_;

    // Device-side arrays
    void       **d_args_;       // device pointers to argument data
    char        *d_arg_data_;   // contiguous device argument storage
    size_t       d_arg_capacity_;

    // CUDA resources
    cudaStream_t streams_[CSMV_BATCH_STREAM_COUNT];
    cudaEvent_t  events_[CSMV_BATCH_STREAM_COUNT * 4];
    int          stream_idx_;
    int          active_streams_;
};

// ── Host-side CSMV GPU device lifecycle (declared here so benchmarks can
//    call them; implemented in csmv_batch_executor.cu) ──────────────
extern "C" void csmv_gpu_init(int table_entries);
extern "C" void csmv_gpu_shutdown();

// ── GPU-compatible version list ─────────────────────────────────
//
// Since std::atomic and std::mutex don't work in CUDA device code,
// we use a separate GPU-side table of raw pointers + spinlocks.
// The host code allocates this table and copies the pointer to
// __constant__ memory.
#if defined(__CUDACC__) || defined(__HIPCC__)

#define CSMV_GPU_TABLE_SIZE  (1 << 20)  // 1M entries

// GPU-side entry: raw pointer (accessed via CUDA atomics) + lock byte
struct CSMVGpuEntry {
    uintptr_t head;   // CSMVVersionNode* as integer for CUDA atomics
    int       lock;   // 0=free, 1=locked
};

// Device-visible symbols are owned by csmv_batch_executor.cu (single TU) and
// exposed via accessor functions below.  Do NOT declare `extern __device__`
// globals here: nvcc treats such a header declaration as a definition
// (warning #20044-D), causing duplicate-symbol errors when the .cu also
// defines the variable.  Function declarations are immune to that trap.
__device__ CSMVGpuEntry* csmv_gpu_table();
__device__ uint64_t*    csmv_gpu_clock_addr();

__device__ inline uint64_t csmv_gpu_atomic_load_clock() {
    return atomicAdd((unsigned long long*)csmv_gpu_clock_addr(), 0ULL);
}

__device__ inline uint64_t csmv_gpu_atomic_inc_clock() {
    return atomicAdd((unsigned long long*)csmv_gpu_clock_addr(), 1ULL);
}

__device__ inline CSMVVersionNode* csmv_gpu_load_head(CSMVGpuEntry *entry) {
    uintptr_t v = atomicAdd((unsigned long long*)&entry->head, 0ULL);
    return (CSMVVersionNode*)v;
}

__device__ inline void csmv_gpu_store_head(CSMVGpuEntry *entry, CSMVVersionNode *node) {
    atomicExch((unsigned long long*)&entry->head, (unsigned long long)(uintptr_t)node);
}

// Per-warp shared state (allocated in shared memory by the kernel)
struct CSMVWarpState {
    uint64_t start_clock;
    int      num_reads;
    int      num_writes;
    struct { uint64_t entry_idx; uint64_t observed_ts; } reads[CSMV_MAX_READS];
    struct { uint64_t entry_idx; uint64_t val; } writes[CSMV_MAX_WRITES];
};

// Look up GPU entry index from data address
__device__ inline uint64_t csmv_gpu_entry_idx(void *data_addr) {
    return ((uintptr_t)data_addr >> 3) & (CSMV_GPU_TABLE_SIZE - 1);
}

// Warp-cooperative read: all lanes search the version list in parallel.
// `inline` so that under -rdc=true (required because csmv_gpu_table() lives
// in a separate TU) each including TU gets its own copy — otherwise nvlink
// reports a multiple-definition error for these header-defined device funcs.
__device__ inline uint64_t csmv_gpu_read(CSMVWarpState *ws, void *data_addr) {
    uint64_t idx = csmv_gpu_entry_idx(data_addr);

    // Check write-set first
    for (int i = 0; i < ws->num_writes; i++) {
        if (ws->writes[i].entry_idx == idx)
            return ws->writes[i].val;
    }

    CSMVGpuEntry *entry = &csmv_gpu_table()[idx];

    // Warp-cooperative version list traversal.  The list is newest-first, so
    // the *first* node with ts <= start_clock is the newest value visible to
    // this snapshot.  All lanes follow the identical chain (same head, same
    // next pointers), so they diverge together; lane 0 records the value and
    // the *node's own* timestamp (NOT the head timestamp) as the observed
    // version for validation.  Using the head timestamp is a bug: if a
    // concurrent commit prepends a node with ts > start_clock, we read the
    // older visible node's value but record the newer head's ts, letting a
    // stale read-modify-write pass validation (lost update).
    CSMVVersionNode *node = csmv_gpu_load_head(entry);
    uint64_t result = 0;
    uint64_t observed = 0;
    uint64_t lane_mask = __activemask();
    int lane_id = threadIdx.x & 31;

    while (node) {
        if (node->timestamp <= ws->start_clock) {
            if (lane_id == 0) {
                result = node->value;
                observed = node->timestamp;
            }
            break;
        }
        node = node->next;
    }
    result   = __shfl_sync(lane_mask, result, 0);
    observed = __shfl_sync(lane_mask, observed, 0);

    // Record read for validation (lane 0)
    if (ws->num_reads < CSMV_MAX_READS && lane_id == 0) {
        ws->reads[ws->num_reads].entry_idx = idx;
        ws->reads[ws->num_reads].observed_ts = observed;
        ws->num_reads++;
    }

    return result;
}

__device__ inline void csmv_gpu_write(CSMVWarpState *ws, void *data_addr, uint64_t val) {
    // Only lane 0 maintains the shared write-set; all lanes must call
    // this function (for warp convergence) but only lane 0 mutates it.
    if ((threadIdx.x & 31) != 0) return;
    uint64_t idx = csmv_gpu_entry_idx(data_addr);

    for (int i = 0; i < ws->num_writes; i++) {
        if (ws->writes[i].entry_idx == idx) {
            ws->writes[i].val = val;
            return;
        }
    }
    if (ws->num_writes < CSMV_MAX_WRITES) {
        ws->writes[ws->num_writes].entry_idx = idx;
        ws->writes[ws->num_writes].val = val;
        ws->num_writes++;
    }
}

__device__ inline uint64_t csmv_gpu_begin(CSMVWarpState *ws) {
    ws->start_clock = csmv_gpu_atomic_load_clock();
    ws->num_reads = 0;
    ws->num_writes = 0;
    return ws->start_clock;
}

__device__ inline uint64_t csmv_gpu_commit(CSMVWarpState *ws) {
    uint64_t lane_mask = __activemask();
    int lane_id = threadIdx.x & 31;
    int num_writes = ws->num_writes;
    int num_reads = ws->num_reads;

    // Fast-path optimistic read-set validation (no locks).
    int fail = 0;
    if (lane_id == 0) {
        for (int i = 0; i < num_reads; i++) {
            CSMVGpuEntry *entry = &csmv_gpu_table()[ws->reads[i].entry_idx];
            CSMVVersionNode *head = csmv_gpu_load_head(entry);
            uint64_t head_ts = head ? head->timestamp : 0;
            if (head_ts != ws->reads[i].observed_ts) { fail = 1; break; }
        }
    }
    if (__ballot_sync(lane_mask, fail)) return 0;

    // Acquire per-entry locks on every write-set entry (lane 0).  We never
    // spin: on contention we release and abort, so no deadlock is possible.
    // This closes the validate-vs-prepend race (two txns reading the same
    // record, both passing validation before either prepends) that otherwise
    // loses increments when both commits are counted but only one node's
    // value survives as the head.
    // NOTE: `fail` must be set only by lane 0 (the validation loops use the
    // same pattern); do NOT ballot on a per-lane counter like `locked`, which
    // is 0 on lanes 1-31 and would make every write tx falsely abort.
    int lock_fail = 0;
    int locked = 0;
    if (lane_id == 0) {
        for (int i = 0; i < num_writes; i++) {
            CSMVGpuEntry *entry = &csmv_gpu_table()[ws->writes[i].entry_idx];
            if (atomicCAS(&entry->lock, 0, 1) != 0) { lock_fail = 1; break; }
            locked = i + 1;
        }
    }
    if (__ballot_sync(lane_mask, lock_fail)) {
        if (lane_id == 0) {
            for (int i = 0; i < locked; i++)
                atomicExch(&csmv_gpu_table()[ws->writes[i].entry_idx].lock, 0);
        }
        __threadfence();
        return 0;
    }

    // Re-validate the read-set under the write locks: between the fast-path
    // validation and acquiring the locks, a concurrent commit could have
    // changed a read entry.  Any read entry that is also a locked write entry
    // is safe (we hold its lock).
    fail = 0;
    if (lane_id == 0) {
        for (int i = 0; i < num_reads; i++) {
            CSMVGpuEntry *entry = &csmv_gpu_table()[ws->reads[i].entry_idx];
            CSMVVersionNode *head = csmv_gpu_load_head(entry);
            uint64_t head_ts = head ? head->timestamp : 0;
            if (head_ts != ws->reads[i].observed_ts) { fail = 1; break; }
        }
    }
    if (__ballot_sync(lane_mask, fail)) {
        if (lane_id == 0) {
            for (int i = 0; i < num_writes; i++)
                atomicExch(&csmv_gpu_table()[ws->writes[i].entry_idx].lock, 0);
        }
        __threadfence();
        return 0;
    }

    // Increment the global clock once (lane 0), broadcast the result, then
    // prepend every write node and release its lock.
    uint64_t commit_ts;
    if (lane_id == 0) {
        __threadfence();  // ensure all prior reads complete
        commit_ts = csmv_gpu_atomic_inc_clock() + 1;
        for (int i = 0; i < num_writes; i++) {
            CSMVGpuEntry *entry = &csmv_gpu_table()[ws->writes[i].entry_idx];
            CSMVVersionNode *node = (CSMVVersionNode*)malloc(sizeof(CSMVVersionNode));
            node->timestamp = commit_ts;
            node->value = ws->writes[i].val;
            node->next = csmv_gpu_load_head(entry);
            csmv_gpu_store_head(entry, node);
            atomicExch(&entry->lock, 0);  // release
        }
    }
    commit_ts = __shfl_sync(lane_mask, commit_ts, 0);
    return commit_ts;
}

// ── Persistent GPU kernel for batch execution ──────────────────
// Defined in csmv_batch_executor.cu (single TU) — declared here so the host
// launcher can reference it.  A __global__ in a header included by several
// TUs would collide under -rdc=true.
__global__ void csmv_batch_kernel(csmv_tx_body_t fn, void **args, int num_txns);

#endif // __CUDACC__ / __HIPCC__
