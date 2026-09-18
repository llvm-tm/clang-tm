#include "gpu_gust_batch_executor.cuh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

// ── Device-side state ──────────────────────────────────────────
// Allocated by gust_gpu_init(), pointer stored in __device__ globals
// (accessor functions avoid the nvcc header-definition trap).
__device__ GUSTVBox    *g_gust_vboxes = nullptr;
__device__ uint64_t    *g_gust_gts = nullptr;
__device__ uint64_t    *g_gust_write_ptr = nullptr;
__device__ GUSTCLEntry *g_gust_cl = nullptr;
__device__ uint64_t    *g_gust_committed = nullptr;
__device__ uint64_t    *g_gust_aborted = nullptr;

__device__ GUSTVBox*    gust_gpu_vboxes()   { return g_gust_vboxes; }
__device__ uint64_t*    gust_gpu_gts()      { return g_gust_gts; }
__device__ uint64_t*    gust_gpu_write_ptr(){ return g_gust_write_ptr; }
__device__ GUSTCLEntry* gust_gpu_cl()       { return g_gust_cl; }
__device__ uint64_t*    gust_gpu_committed(){ return g_gust_committed; }
__device__ uint64_t*    gust_gpu_aborted()  { return g_gust_aborted; }

// ── Batch kernel: one warp per 32 transactions, one tx per lane ─
// `num_txns` must be a multiple of WARP_SIZE (validated by host).
__global__ void gpu_gust_batch_kernel(gust_tx_body_t fn, void **args,
                                      int num_txns) {
    const int lane = threadIdx.x;
    const int warp = blockIdx.x;
    const int txn  = warp * GPU_GUST_WARP_SIZE + lane;
    if (txn >= num_txns) return;

    extern __shared__ char s_buf[];
    GUSTWarpState *ws_array = (GUSTWarpState*)s_buf;   // [32] per-lane states
    GUSTWarpState *ws = &ws_array[lane];

    // Kernel performs the snapshot so every lane begins identically;
    // the body then issues reads/writes and must call gust_gpu_commit.
    gust_gpu_begin(ws);
    __syncwarp();

    fn(lane, warp, args[txn], ws);
    __syncwarp();
}

// ── Snapshot kernel: newest committed value per vbox ───────────
__global__ void gust_gpu_snapshot_kernel(uint32_t *out, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        uint64_t ver;
        uint32_t val;
        uint64_t gts_now = atomicAdd((unsigned long long*)g_gust_gts, 0ULL);
        gpu_gust_vbox_read_value(&g_gust_vboxes[i], gts_now, &ver, &val);
        out[i] = val;
    }
}

// ── GUSTBatchExecutor implementation ───────────────────────────

GUSTBatchExecutor::GUSTBatchExecutor()
    : d_args_(nullptr), d_arg_data_(nullptr), d_arg_capacity_(0), stream_idx_(0)
{
}

GUSTBatchExecutor::~GUSTBatchExecutor() {
    if (d_args_) cudaFree(d_args_);
    if (d_arg_data_) cudaFree(d_arg_data_);
}

void GUSTBatchExecutor::enqueue(gust_tx_body_t fn, void *arg, size_t arg_size) {
    GUSTBatchWorkItem item;
    item.fn = fn;
    item.arg_size = arg_size;
    item.arg = nullptr;
    item.host_arg_copy.assign((const uint8_t*)arg, (const uint8_t*)arg + arg_size);
    batch_.push_back(item);
}

GUSTBatchExecutor::BatchTiming GUSTBatchExecutor::launch() {
    BatchTiming timing = {0, 0, 0, 0};
    int n = (int)batch_.size();
    if (n == 0) return timing;

    // GUST publication protocol advances GTS by a full warp per batch;
    // partial warps would desynchronize the __ballot_sync in commit.
    assert(n % GPU_GUST_WARP_SIZE == 0);
    if (n % GPU_GUST_WARP_SIZE != 0) {
        fprintf(stderr, "[GUST-Batch] num_txns (%d) must be a multiple of %d\n",
                n, GPU_GUST_WARP_SIZE);
        abort();
    }

    // ── Allocate device argument storage ──────────────────────
    size_t total_arg_size = 0;
    for (auto &item : batch_)
        total_arg_size += item.arg_size;

    if (total_arg_size > d_arg_capacity_) {
        if (d_arg_data_) cudaFree(d_arg_data_);
        CUDA_CHECK(cudaMalloc(&d_arg_data_, total_arg_size));
        d_arg_capacity_ = total_arg_size;
    }
    if (d_args_) cudaFree(d_args_);
    CUDA_CHECK(cudaMalloc(&d_args_, n * sizeof(void*)));

    // ── Copy argument data H2D, build device pointer array ────
    std::vector<void*> h_args(n);
    size_t offset = 0;
    for (int i = 0; i < n; i++) {
        if (batch_[i].arg_size > 0) {
            CUDA_CHECK(cudaMemcpy(d_arg_data_ + offset, batch_[i].host_arg_copy.data(),
                                  batch_[i].arg_size, cudaMemcpyHostToDevice));
        }
        h_args[i] = d_arg_data_ + offset;
        batch_[i].arg = d_arg_data_ + offset;
        offset += batch_[i].arg_size;
    }
    CUDA_CHECK(cudaMemcpy(d_args_, h_args.data(), n * sizeof(void*),
                          cudaMemcpyHostToDevice));

    // ── Launch ────────────────────────────────────────────────
    cudaEvent_t ev_start, ev_stop;
    CUDA_CHECK(cudaEventCreate(&ev_start));
    CUDA_CHECK(cudaEventCreate(&ev_stop));

    int num_warps = n / GPU_GUST_WARP_SIZE;
    size_t shared_bytes = GPU_GUST_WARP_SIZE * sizeof(GUSTWarpState);

    gust_tx_body_t d_fn = batch_[0].fn;
    CUDA_CHECK(cudaEventRecord(ev_start));
    gpu_gust_batch_kernel<<<num_warps, GPU_GUST_WARP_SIZE, shared_bytes>>>(
        d_fn, d_args_, n);
    CUDA_CHECK(cudaEventRecord(ev_stop));
    CUDA_CHECK(cudaDeviceSynchronize());

    CUDA_CHECK(cudaEventElapsedTime(&timing.kernel_ms, ev_start, ev_stop));
    timing.total_ms = timing.kernel_ms;

    CUDA_CHECK(cudaEventDestroy(ev_start));
    CUDA_CHECK(cudaEventDestroy(ev_stop));

    profile_events_.push_back({timing.kernel_ms, 0, 0, n});
    return timing;
}

void GUSTBatchExecutor::synchronize() {
    batch_.clear();
}

// ── Device lifecycle ───────────────────────────────────────────

static int g_num_addrs = 0;

extern "C" void gust_gpu_init(int num_addrs) {
    g_num_addrs = num_addrs;
    // cudaMalloc cannot write a pointer into a __device__ variable (it needs a
    // host void**; the device global would stay null and every kernel that
    // dereferences it faults with an illegal memory access — on BOTH CUDA and
    // HIP).  Allocate host-side pointers and copy the values into the device
    // globals with cudaMemcpyToSymbol (same idiom as csmv_gpu_init). See review-03.
    GUSTVBox    *d_vboxes = nullptr;
    uint64_t    *d_gts = nullptr;
    uint64_t    *d_write_ptr = nullptr;
    GUSTCLEntry *d_cl = nullptr;
    uint64_t    *d_committed = nullptr;
    uint64_t    *d_aborted = nullptr;

    CUDA_CHECK(cudaMalloc(&d_vboxes, num_addrs * sizeof(GUSTVBox)));
    CUDA_CHECK(cudaMemset(d_vboxes, 0, num_addrs * sizeof(GUSTVBox)));
    CUDA_CHECK(cudaMemcpyToSymbol(g_gust_vboxes, &d_vboxes, sizeof(d_vboxes)));

    CUDA_CHECK(cudaMalloc(&d_gts, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_gts, 0, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemcpyToSymbol(g_gust_gts, &d_gts, sizeof(d_gts)));

    CUDA_CHECK(cudaMalloc(&d_write_ptr, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_write_ptr, 0, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemcpyToSymbol(g_gust_write_ptr, &d_write_ptr, sizeof(d_write_ptr)));

    CUDA_CHECK(cudaMalloc(&d_cl, GPU_GUST_CL_SIZE * sizeof(GUSTCLEntry)));
    CUDA_CHECK(cudaMemset(d_cl, 0, GPU_GUST_CL_SIZE * sizeof(GUSTCLEntry)));
    CUDA_CHECK(cudaMemcpyToSymbol(g_gust_cl, &d_cl, sizeof(d_cl)));

    CUDA_CHECK(cudaMalloc(&d_committed, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_committed, 0, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemcpyToSymbol(g_gust_committed, &d_committed, sizeof(d_committed)));

    CUDA_CHECK(cudaMalloc(&d_aborted, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_aborted, 0, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemcpyToSymbol(g_gust_aborted, &d_aborted, sizeof(d_aborted)));
}

extern "C" void gust_gpu_shutdown(void) {
    // Read the device-side pointers back to host before freeing (the device
    // globals hold the values set in gust_gpu_init; &global in host code is not
    // their value).
    GUSTVBox    *d_vboxes = nullptr;
    uint64_t    *d_gts = nullptr;
    uint64_t    *d_write_ptr = nullptr;
    GUSTCLEntry *d_cl = nullptr;
    uint64_t    *d_committed = nullptr;
    uint64_t    *d_aborted = nullptr;
    cudaMemcpyFromSymbol(&d_vboxes, g_gust_vboxes, sizeof(d_vboxes));
    cudaMemcpyFromSymbol(&d_gts, g_gust_gts, sizeof(d_gts));
    cudaMemcpyFromSymbol(&d_write_ptr, g_gust_write_ptr, sizeof(d_write_ptr));
    cudaMemcpyFromSymbol(&d_cl, g_gust_cl, sizeof(d_cl));
    cudaMemcpyFromSymbol(&d_committed, g_gust_committed, sizeof(d_committed));
    cudaMemcpyFromSymbol(&d_aborted, g_gust_aborted, sizeof(d_aborted));
    if (d_vboxes) cudaFree(d_vboxes);
    if (d_gts) cudaFree(d_gts);
    if (d_write_ptr) cudaFree(d_write_ptr);
    if (d_cl) cudaFree(d_cl);
    if (d_committed) cudaFree(d_committed);
    if (d_aborted) cudaFree(d_aborted);
}

extern "C" void gust_gpu_snapshot(uint32_t *h_out, int n) {
    uint32_t *d_out;
    CUDA_CHECK(cudaMalloc(&d_out, n * sizeof(uint32_t)));
    gust_gpu_snapshot_kernel<<<(n + 255) / 256, 256>>>(d_out, n);
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaMemcpy(h_out, d_out, n * sizeof(uint32_t),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaFree(d_out));
}

extern "C" uint64_t gust_gpu_committed_count(void) {
    // g_gust_committed is a __device__ pointer variable; read the pointer value
    // first (FromSymbol), then copy the counter it points to.  Passing the
    // device-symbol address directly to cudaMemcpy is invalid on HIP.
    uint64_t *d_ptr = nullptr;
    cudaMemcpyFromSymbol(&d_ptr, g_gust_committed, sizeof(d_ptr));
    uint64_t v = 0;
    if (d_ptr) CUDA_CHECK(cudaMemcpy(&v, d_ptr, sizeof(uint64_t), cudaMemcpyDeviceToHost));
    return v;
}

extern "C" uint64_t gust_gpu_aborted_count(void) {
    uint64_t *d_ptr = nullptr;
    cudaMemcpyFromSymbol(&d_ptr, g_gust_aborted, sizeof(d_ptr));
    uint64_t v = 0;
    if (d_ptr) CUDA_CHECK(cudaMemcpy(&v, d_ptr, sizeof(uint64_t), cudaMemcpyDeviceToHost));
    return v;
}

// ── Initial-value seeding ──────────────────────────────────────
// Writes the initial value into every VBox (slot 0, version 1, head=1)
// so snapshot reads at any startTS ≥ 1 see the initial committed value.
__global__ void gust_gpu_seed_kernel(const uint32_t *vals, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        g_gust_vboxes[i].values[0]   = vals[i];
        g_gust_vboxes[i].versions[0] = 1;
        g_gust_vboxes[i].head        = 1;
    }
}

extern "C" void gust_gpu_seed(const uint32_t *h_vals, int n) {
    uint32_t *d_vals;
    CUDA_CHECK(cudaMalloc(&d_vals, n * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemcpy(d_vals, h_vals, n * sizeof(uint32_t),
                          cudaMemcpyHostToDevice));
    gust_gpu_seed_kernel<<<(n + 255) / 256, 256>>>(d_vals, n);
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaFree(d_vals));
}
