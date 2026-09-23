#include "tm_gpu_platform.hpp"
#include "csmv_kernel.cuh"

#if defined(__CUDACC__) || defined(__HIPCC__)

// ── Device-side CSMV state ──────────────────────────────────────
// The object table and version clock live in device global memory,
// allocated by cudaMalloc and managed via csmv_tm_init().

__device__ __constant__ CSMVObjectEntry *d_csmv_table = nullptr;
__device__ __constant__ std::atomic<uint64_t> *d_csmv_clock = nullptr;
__device__ std::atomic<uint64_t> d_csmv_device_clock{0};

// ── Device helpers ──────────────────────────────────────────────

__device__ inline CSMVObjectEntry* csmv_get_entry(void *addr) {
    uint64_t idx = ((uintptr_t)addr >> 3) & (CSMV_TABLE_SIZE - 1);
    return &d_csmv_table[idx];
}

// ── Read: warp-cooperative version list traversal ──────────────
// Lane 0 loads head.  All lanes walk the list in parallel:
// lane i checks node i.  __ballot_sync finds the first match.
// __shfl_sync broadcasts the value.

__device__ uint64_t csmv_device_read(void *data_addr, uint64_t start_clock) {
    CSMVObjectEntry *entry = csmv_get_entry(data_addr);
    CSMVVersionNode *node = (CSMVVersionNode*)(uintptr_t)
        __atomic_load_n((uintptr_t*)&entry->head, __ATOMIC_SEQ_CST);

    while (node) {
        uint64_t ts = node->timestamp;
        uint64_t val = node->value;
        // Lane 0 checks the current node
        int match = (threadIdx.x == 0 && ts <= start_clock) ? 1 : 0;
        int any_match = __any_sync(~0ULL, match);
        if (any_match) {
            // Broadcast matching value from lane 0
            return __shfl_sync(~0ULL, val, 0);
        }
        // Move to next node (older version)
        node = node->next;
    }
    return 0; // no version found (shouldn't happen with proper init)
}

// ── Write: create new version node at commit ───────────────────

__device__ void csmv_device_write(CSMVObjectEntry *entry,
                                   uint64_t commit_ts,
                                   uint64_t value) {
    // Leader (lane 0) allocates and prepends the version node
    if (threadIdx.x == 0) {
        CSMVVersionNode *new_node = (CSMVVersionNode*)malloc(sizeof(CSMVVersionNode));
        new_node->timestamp = commit_ts;
        new_node->value = value;
        new_node->next = (CSMVVersionNode*)(uintptr_t)
            __atomic_load_n((uintptr_t*)&entry->head, __ATOMIC_SEQ_CST);
        __atomic_store_n((uintptr_t*)&entry->head, (uintptr_t)new_node, __ATOMIC_SEQ_CST);
    }
    __syncwarp();
}

// ── Device-side begin / end ────────────────────────────────────

__device__ uint64_t csmv_device_begin() {
    return atomicAdd((unsigned long long*)d_csmv_clock, 0);
}

__device__ uint64_t csmv_device_commit() {
    return atomicAdd((unsigned long long*)d_csmv_clock, 1) + 1;
}

// ── Persistent kernel ──────────────────────────────────────────

__global__ void csmv_persistent_kernel(
        int num_warps,
        void (*tx_body)(int lane_id, int warp_id,
                        void *data, void *shared_scratch),
        void *tx_data) {
    __shared__ __align__(256) uint8_t shared_scratch[16384];

    int warp_id = blockIdx.x * blockDim.x / warpSize + threadIdx.x / warpSize;
    int lane_id = threadIdx.x & (warpSize - 1);

    if (warp_id >= num_warps) return;

    // Each warp runs one transaction (leader drives the protocol)
    tx_body(lane_id, warp_id, tx_data, (void*)shared_scratch);
}

// ── Host launch wrapper ─────────────────────────────────────────

int csmv_gpu_launch(int num_warps,
                     void (*tx_body)(int, int, void*, void*),
                     void *tx_data) {
    int threads_per_block = 128;
    int blocks = (num_warps * 32 + threads_per_block - 1) / threads_per_block;

    csmv_persistent_kernel<<<blocks, threads_per_block>>>(
            num_warps, tx_body, tx_data);

    cudaDeviceSynchronize();
    return cudaGetLastError() == cudaSuccess ? num_warps : 0;
}

#endif // __CUDACC__ / __HIPCC__
