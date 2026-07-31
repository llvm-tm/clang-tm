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
    void           *host_arg; // host-side argument (copied to device)
    size_t          arg_size; // size of argument in bytes
};

// ── Host-side batch executor ────────────────────────────────────

class CSMVBatchExecutor {
public:
    CSMVBatchExecutor();
    ~CSMVBatchExecutor();

    // Enqueue a transaction.  arg is copied to device-side storage.
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

// Device-visible symbols set by csmv_gpu_init()
extern __device__ __constant__ CSMVGpuEntry *g_csmv_gpu_head_table;
extern __device__ __constant__ uint64_t     *g_csmv_gpu_clock;

// Host lifecycle (defined in csmv_batch_executor.cu)
extern "C" void csmv_gpu_init(int table_entries);
extern "C" void csmv_gpu_shutdown();

__device__ inline uint64_t csmv_gpu_atomic_load_clock() {
    return atomicAdd((unsigned long long*)g_csmv_gpu_clock, 0ULL);
}

__device__ inline uint64_t csmv_gpu_atomic_inc_clock() {
    return atomicAdd((unsigned long long*)g_csmv_gpu_clock, 1ULL);
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
__device__ uint64_t csmv_gpu_read(CSMVWarpState *ws, void *data_addr) {
    uint64_t idx = csmv_gpu_entry_idx(data_addr);

    // Check write-set first
    for (int i = 0; i < ws->num_writes; i++) {
        if (ws->writes[i].entry_idx == idx)
            return ws->writes[i].val;
    }

    CSMVGpuEntry *entry = &g_csmv_gpu_head_table[idx];

    // Warp-cooperative version list traversal
    CSMVVersionNode *node = csmv_gpu_load_head(entry);
    uint64_t result = 0;
    uint64_t lane_mask = __activemask();
    int lane_id = threadIdx.x & 31;

    while (node) {
        if (lane_id == 0 && node->timestamp <= ws->start_clock) {
            result = node->value;
        }
        uint64_t found = __ballot_sync(lane_mask, result != 0);
        if (found) {
            result = __shfl_sync(lane_mask, result, 0);
            break;
        }
        node = node->next;
    }

    // Record read for validation (lane 0)
    if (ws->num_reads < CSMV_MAX_READS && lane_id == 0) {
        CSMVVersionNode *head = csmv_gpu_load_head(entry);
        ws->reads[ws->num_reads].entry_idx = idx;
        ws->reads[ws->num_reads].observed_ts = head ? head->timestamp : 0;
        ws->num_reads++;
    }

    return result;
}

__device__ void csmv_gpu_write(CSMVWarpState *ws, void *data_addr, uint64_t val) {
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

__device__ uint64_t csmv_gpu_begin(CSMVWarpState *ws) {
    ws->start_clock = csmv_gpu_atomic_load_clock();
    ws->num_reads = 0;
    ws->num_writes = 0;
    return ws->start_clock;
}

__device__ uint64_t csmv_gpu_commit(CSMVWarpState *ws) {
    uint64_t lane_mask = __activemask();
    int lane_id = threadIdx.x & 31;

    // Validate read-set: lane 0 checks head timestamps, result is
    // broadcast to all lanes via ballot (no early return, so the warp
    // stays converged for the ballot).
    int fail = 0;
    if (lane_id == 0) {
        for (int i = 0; i < ws->num_reads; i++) {
            CSMVGpuEntry *entry = &g_csmv_gpu_head_table[ws->reads[i].entry_idx];
            CSMVVersionNode *head = csmv_gpu_load_head(entry);
            uint64_t head_ts = head ? head->timestamp : 0;
            if (head_ts != ws->reads[i].observed_ts) { fail = 1; break; }
        }
    }
    if (__ballot_sync(lane_mask, fail)) return 0;

    // Increment the global clock once (lane 0), broadcast the result.
    uint64_t commit_ts;
    if (lane_id == 0) {
        __threadfence();  // ensure all prior reads complete
        commit_ts = csmv_gpu_atomic_inc_clock() + 1;
    }
    commit_ts = __shfl_sync(lane_mask, commit_ts, 0);

    if (lane_id == 0) {
        for (int i = 0; i < ws->num_writes; i++) {
            CSMVGpuEntry *entry = &g_csmv_gpu_head_table[ws->writes[i].entry_idx];
            CSMVVersionNode *node = (CSMVVersionNode*)malloc(sizeof(CSMVVersionNode));
            node->timestamp = commit_ts;
            node->value = ws->writes[i].val;
            node->next = csmv_gpu_load_head(entry);
            csmv_gpu_store_head(entry, node);
        }
    }
    return commit_ts;
}

// ── Persistent GPU kernel for batch execution ──────────────────

__global__ void csmv_batch_kernel(
    csmv_tx_body_t *fns,
    void          **args,
    int             num_txns)
{
    // Shared memory: first part is CSMVWarpState array, rest is scratch
    // (must be at least CSMV_BATCH_WARPS_PER_BLOCK * sizeof(CSMVWarpState) + scratch)
    __shared__ uint8_t shared_mem[
        CSMV_BATCH_WARPS_PER_BLOCK * sizeof(CSMVWarpState) + 4096];

    int warp_id = blockIdx.x * blockDim.x / warpSize + threadIdx.x / warpSize;
    int lane_id = threadIdx.x & (warpSize - 1);

    if (warp_id >= num_txns) return;

    CSMVWarpState *ws_array = (CSMVWarpState*)shared_mem;
    CSMVWarpState *ws = &ws_array[threadIdx.x / warpSize];
    void *scratch = shared_mem + CSMV_BATCH_WARPS_PER_BLOCK * sizeof(CSMVWarpState);

    // Pass the warp's own CSMVWarpState to the tx body as shared_scratch,
    // so transaction code can call csmv_gpu_begin/read/write/commit directly.
    fns[warp_id](lane_id, warp_id, args[warp_id], ws);
}

#endif // __CUDACC__ / __HIPCC__
