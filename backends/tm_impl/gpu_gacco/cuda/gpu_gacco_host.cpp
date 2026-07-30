// GPU GAccO host-only TM hooks
#include "tm_gpu_platform.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <mutex>

#include "gpu_gacco_api.h"
#include "tm_hooks.hpp"
#include "tm_region_allocator.hpp"
#include "tm_thread_state.hpp"

uint32_t *g_gpu_gacco_lock_table = nullptr;
uint64_t *g_gpu_gacco_clock = nullptr;
uint32_t *g_gpu_gacco_data = nullptr;
int       g_gpu_gacco_num_addrs = 1024;
uint64_t *g_gpu_gacco_committed = nullptr;
uint64_t *g_gpu_gacco_aborted = nullptr;

int gpu_available = 0;
static std::mutex init_mutex;
static thread_local int tl_in_tx = 0;

extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

static int check_gpu() {
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    return (err == cudaSuccess && count > 0) ? 1 : 0;
}

static void init_device_memory() {
    CUDA_CHECK(cudaMalloc(&g_gpu_gacco_lock_table,
               GPU_GACCO_LOCKTABLE_SIZE * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemset(g_gpu_gacco_lock_table, 0,
               GPU_GACCO_LOCKTABLE_SIZE * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&g_gpu_gacco_clock, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(g_gpu_gacco_clock, 0, sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&g_gpu_gacco_data,
               g_gpu_gacco_num_addrs * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemset(g_gpu_gacco_data, 0,
               g_gpu_gacco_num_addrs * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&g_gpu_gacco_committed, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(g_gpu_gacco_committed, 0, sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&g_gpu_gacco_aborted, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(g_gpu_gacco_aborted, 0, sizeof(uint64_t)));
}

void gpu_gacco_init() {
    std::lock_guard<std::mutex> lock(init_mutex);
    if (stm::tm_region_init() != 0) { abort(); }
    gpu_available = check_gpu();
    g_gpu_gacco_num_addrs = 1024;
    if (gpu_available) {
        cudaSetDevice(0);
        init_device_memory();
        printf("[GPU-GAccO] GPU available.\n");
    } else {
        printf("[GPU-GAccO] No GPU.\n");
    }
    extern const TMRealHooks g_gpu_gacco_hooks;
    tm_register_real_hooks(&g_gpu_gacco_hooks);
}

void gpu_gacco_exit() {
    if (gpu_available) {
        cudaFree(g_gpu_gacco_lock_table);
        cudaFree(g_gpu_gacco_clock);
        cudaFree(g_gpu_gacco_data);
        cudaFree(g_gpu_gacco_committed);
        cudaFree(g_gpu_gacco_aborted);
        cudaDeviceReset();
    }
}

void gpu_gacco_init_thread() { tm_hook_init_thread(); }
void gpu_gacco_exit_thread() {}
void gpu_gacco_begin() { tl_in_tx = 1; }
void gpu_gacco_end()   { tl_in_tx = 0; }

void *gpu_gacco_malloc(size_t sz) {
    void *p;
    if (gpu_available) { CUDA_CHECK(cudaMallocManaged(&p, sz)); }
    else               { p = stm::tm_region_malloc(sz); }
    if (p) memset(p, 0, sz);
    return p;
}

void gpu_gacco_free(void *p) {
    if (!p) return;
    if (gpu_available) cudaFree(p);
    else               stm::tm_region_free(p);
}

void *gpu_gacco_calloc(size_t nmemb, size_t sz) {
    size_t total = nmemb * sz;
    void *p = gpu_gacco_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *gpu_gacco_realloc(void *p, size_t sz) {
    if (!p) return gpu_gacco_malloc(sz);
    void *n = gpu_gacco_malloc(sz);
    if (n && sz > 0) { memcpy(n, p, sz); gpu_gacco_free(p); }
    return n;
}

uint8_t  gpu_gacco_read_i1(uint8_t *addr)  { return *addr; }
uint16_t gpu_gacco_read_i2(uint16_t *addr) { return *addr; }
uint32_t gpu_gacco_read_i4(uint32_t *addr) { return *addr; }
uint64_t gpu_gacco_read_i8(uint64_t *addr) { return *addr; }
float    gpu_gacco_read_f4(float *addr)    { return *addr; }
double   gpu_gacco_read_f8(double *addr)   { return *addr; }
void    *gpu_gacco_read_ptr(void **addr)   { return *addr; }

void gpu_gacco_write_i1(uint8_t *addr, uint8_t val)  { *addr = val; }
void gpu_gacco_write_i2(uint16_t *addr, uint16_t val) { *addr = val; }
void gpu_gacco_write_i4(uint32_t *addr, uint32_t val) { *addr = val; }
void gpu_gacco_write_i8(uint64_t *addr, int64_t val)  { *addr = val; }
void gpu_gacco_write_f4(float *addr, float val)       { *addr = val; }
void gpu_gacco_write_f8(double *addr, double val)     { *addr = val; }
void gpu_gacco_write_ptr(void **addr, void *val)      { *addr = val; }

static void *real_tm_get_thread_state() {
    thread_local static TMThreadState state{0, 0};
    return (void*)&state;
}

const TMRealHooks g_gpu_gacco_hooks = {
    .begin    = gpu_gacco_begin,
    .end      = gpu_gacco_end,
    .malloc   = gpu_gacco_malloc,
    .calloc   = gpu_gacco_calloc,
    .realloc  = gpu_gacco_realloc,
    .free     = gpu_gacco_free,
    .read_i1  = gpu_gacco_read_i1,
    .read_i2  = gpu_gacco_read_i2,
    .read_i4  = gpu_gacco_read_i4,
    .read_i8  = gpu_gacco_read_i8,
    .read_f4  = gpu_gacco_read_f4,
    .read_f8  = gpu_gacco_read_f8,
    .read_ptr = gpu_gacco_read_ptr,
    .write_i1  = gpu_gacco_write_i1,
    .write_i2  = gpu_gacco_write_i2,
    .write_i4  = gpu_gacco_write_i4,
    .write_i8  = gpu_gacco_write_i8,
    .write_f4  = gpu_gacco_write_f4,
    .write_f8  = gpu_gacco_write_f8,
    .write_ptr = gpu_gacco_write_ptr,
    .get_thread_state = real_tm_get_thread_state,
};
