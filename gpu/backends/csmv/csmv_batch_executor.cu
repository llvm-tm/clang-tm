#include "csmv_batch_executor.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

// ── Device-side GPU table (allocated by host, pointer stored in __constant__) ──

__device__ CSMVGpuEntry *g_csmv_gpu_head_table = nullptr;
__device__ uint64_t     *g_csmv_gpu_clock = nullptr;

__device__ CSMVGpuEntry* csmv_gpu_table() { return g_csmv_gpu_head_table; }
__device__ uint64_t*    csmv_gpu_clock_addr() { return g_csmv_gpu_clock; }

// ── Persistent GPU kernel for batch execution ──────────────────
// Single device function pointer `fn` (the stored procedure shared by every
// transaction in the batch); each warp runs one transaction with its own arg.
__global__ void csmv_batch_kernel(csmv_tx_body_t fn, void **args, int num_txns) {
    __shared__ uint8_t shared_mem[
        CSMV_BATCH_WARPS_PER_BLOCK * sizeof(CSMVWarpState) + 4096];

    int warp_id = blockIdx.x * blockDim.x / warpSize + threadIdx.x / warpSize;
    int lane_id = threadIdx.x & (warpSize - 1);

    if (warp_id >= num_txns) return;

    CSMVWarpState *ws_array = (CSMVWarpState*)shared_mem;
    CSMVWarpState *ws = &ws_array[threadIdx.x / warpSize];

    fn(lane_id, warp_id, args[warp_id], ws);
}

// ── CSMVBatchExecutor implementation ─────────────────────────────

CSMVBatchExecutor::CSMVBatchExecutor()
    : d_args_(nullptr)
    , d_arg_data_(nullptr)
    , d_arg_capacity_(0)
    , stream_idx_(0)
    , active_streams_(0)
{
    for (int i = 0; i < CSMV_BATCH_STREAM_COUNT; i++) {
        cudaStreamCreate(&streams_[i]);
        for (int j = 0; j < 4; j++)
            cudaEventCreate(&events_[i * 4 + j]);
    }
}

CSMVBatchExecutor::~CSMVBatchExecutor() {
    for (int i = 0; i < CSMV_BATCH_STREAM_COUNT; i++) {
        for (int j = 0; j < 4; j++)
            cudaEventDestroy(events_[i * 4 + j]);
        cudaStreamDestroy(streams_[i]);
    }
    if (d_args_) cudaFree(d_args_);
    if (d_arg_data_) cudaFree(d_arg_data_);
}

void CSMVBatchExecutor::enqueue(csmv_tx_body_t fn, void *arg, size_t arg_size) {
    CSMVBatchWorkItem item;
    item.fn = fn;
    item.arg_size = arg_size;
    item.arg = nullptr;
    item.host_arg_copy.assign((const uint8_t*)arg, (const uint8_t*)arg + arg_size);
    batch_.push_back(item);
}

CSMVBatchExecutor::BatchTiming CSMVBatchExecutor::launch() {
    BatchTiming timing = {0, 0, 0, 0};
    int n = (int)batch_.size();
    if (n == 0) return timing;

    cudaStream_t stream = streams_[stream_idx_ % CSMV_BATCH_STREAM_COUNT];
    cudaEvent_t *ev = &events_[(stream_idx_ % CSMV_BATCH_STREAM_COUNT) * 4];

    stream_idx_++;
    active_streams_++;

    // ── Allocate device memory ──────────────────────────────────
    size_t total_arg_size = 0;
    for (auto &item : batch_)
        total_arg_size += item.arg_size;

    if (total_arg_size > d_arg_capacity_) {
        if (d_arg_data_) cudaFree(d_arg_data_);
        cudaMalloc(&d_arg_data_, total_arg_size);
        d_arg_capacity_ = total_arg_size;
    }
    if (d_args_) cudaFree(d_args_);
    cudaMalloc(&d_args_, n * sizeof(void*));

    // ── Copy argument data H2D (async) ──────────────────────────
    cudaEventRecord(ev[0], stream);
    size_t offset = 0;
    std::vector<void*> h_args(n);
    for (int i = 0; i < n; i++) {
        if (batch_[i].arg_size > 0) {
            cudaMemcpyAsync(d_arg_data_ + offset, batch_[i].host_arg_copy.data(),
                             batch_[i].arg_size, cudaMemcpyHostToDevice, stream);
        }
        h_args[i] = d_arg_data_ + offset;
        batch_[i].arg = d_arg_data_ + offset;
        offset += batch_[i].arg_size;
    }
    // d_args_ is a device array of per-transaction argument pointers; build
    // it from a host array of offsets into d_arg_data_ (NOT the raw bytes).
    cudaMemcpyAsync(d_args_, h_args.data(), n * sizeof(void*),
                     cudaMemcpyHostToDevice, stream);
    cudaEventRecord(ev[1], stream);

    // ── Launch kernel ───────────────────────────────────────────
    // All transactions share one stored-procedure device function pointer.
    csmv_tx_body_t d_fn = batch_[0].fn;
    int warps_per_block = CSMV_BATCH_WARPS_PER_BLOCK;
    int threads_per_block = warps_per_block * 32;
    int blocks = (n + warps_per_block - 1) / warps_per_block;

    cudaEventRecord(ev[2], stream);
    csmv_batch_kernel<<<blocks, threads_per_block, 0, stream>>>(d_fn, d_args_, n);
    cudaEventRecord(ev[3], stream);

    // ── Synchronize and measure ─────────────────────────────────
    cudaStreamSynchronize(stream);

    float ms_h2d, ms_kernel, ms_d2h;
    cudaEventElapsedTime(&ms_h2d, ev[0], ev[1]);
    cudaEventElapsedTime(&ms_kernel, ev[2], ev[3]);

    float total_ms;
    cudaEventElapsedTime(&total_ms, ev[0], ev[3]);

    timing.h2d_ms = ms_h2d;
    timing.kernel_ms = ms_kernel;
    timing.d2h_ms = total_ms - ms_h2d - ms_kernel;
    timing.total_ms = total_ms;

    profile_events_.push_back({ms_kernel, ms_h2d, timing.d2h_ms, n});

    return timing;
}

void CSMVBatchExecutor::synchronize() {
    batch_.clear();
}

// ── CSMV GPU initialization ─────────────────────────────────────

extern "C" void csmv_gpu_init(int table_entries) {
    if (table_entries <= 0)
        table_entries = CSMV_GPU_TABLE_SIZE;

    // Allocate GPU-side entry table + clock
    CSMVGpuEntry *d_table = nullptr;
    uint64_t     *d_clock = nullptr;
    cudaMalloc(&d_table, table_entries * sizeof(CSMVGpuEntry));
    cudaMalloc(&d_clock, sizeof(uint64_t));
    cudaMemset(d_table, 0, table_entries * sizeof(CSMVGpuEntry));
    cudaMemset(d_clock, 0, sizeof(uint64_t));

    // Store pointers in __constant__ memory for fast device access
    cudaMemcpyToSymbol(g_csmv_gpu_head_table, &d_table, sizeof(CSMVGpuEntry*));
    cudaMemcpyToSymbol(g_csmv_gpu_clock,     &d_clock, sizeof(uint64_t*));
}

extern "C" void csmv_gpu_shutdown() {
    CSMVGpuEntry *d_table;
    uint64_t     *d_clock;
    cudaMemcpyFromSymbol(&d_table, g_csmv_gpu_head_table, sizeof(CSMVGpuEntry*));
    cudaMemcpyFromSymbol(&d_clock, g_csmv_gpu_clock, sizeof(uint64_t*));
    if (d_table) cudaFree(d_table);
    if (d_clock) cudaFree(d_clock);
}
