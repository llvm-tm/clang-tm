#include "csmv_batch_executor.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

// ── Device-side GPU table (allocated by host, pointer stored in __constant__) ──

__device__ __constant__ CSMVGpuEntry *g_csmv_gpu_head_table = nullptr;
__device__ __constant__ uint64_t     *g_csmv_gpu_clock = nullptr;

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
    item.host_arg = arg;
    item.arg_size = arg_size;
    item.arg = nullptr;
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

    // Device-side function pointer array
    csmv_tx_body_t *d_fns;
    cudaMalloc(&d_fns, n * sizeof(csmv_tx_body_t));

    // ── Copy argument data H2D (async) ──────────────────────────
    cudaEventRecord(ev[0], stream);
    size_t offset = 0;
    csmv_tx_body_t *h_fns = new csmv_tx_body_t[n];
    for (int i = 0; i < n; i++) {
        cudaMemcpyAsync(d_arg_data_ + offset, batch_[i].host_arg,
                         batch_[i].arg_size, cudaMemcpyHostToDevice, stream);
        batch_[i].arg = (void*)((uintptr_t)d_arg_data_ + offset);
        offset += batch_[i].arg_size;
        h_fns[i] = batch_[i].fn;
    }
    cudaMemcpyAsync(d_args_, d_arg_data_, n * sizeof(void*),
                     cudaMemcpyHostToDevice, stream);
    cudaMemcpyAsync(d_fns, h_fns, n * sizeof(csmv_tx_body_t),
                     cudaMemcpyHostToDevice, stream);
    delete[] h_fns;
    cudaEventRecord(ev[1], stream);

    // ── Launch kernel ───────────────────────────────────────────
    int warps_per_block = CSMV_BATCH_WARPS_PER_BLOCK;
    int threads_per_block = warps_per_block * 32;
    int blocks = (n + warps_per_block - 1) / warps_per_block;

    cudaEventRecord(ev[2], stream);
    csmv_batch_kernel<<<blocks, threads_per_block, 0, stream>>>(d_fns, d_args_, n);
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

    cudaFree(d_fns);

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
