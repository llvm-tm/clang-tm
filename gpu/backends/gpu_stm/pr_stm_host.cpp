// ── PR-STM Host Runtime ─────────────────────────────────────────
// Host-only TM hooks, init/exit, memory management.
// Compiled with g++/clang++ (NOT hipcc/nvcc) so the device linker
// never sees host-side function pointers in g_pr_stm_hooks.
//
// HIP/CUDA API calls:  #include "tm_gpu_platform.hpp" provides
// the cudaMalloc/hipMalloc remapping so calls are portable.

#include "tm_gpu_platform.hpp"
#include "tm_gpu_detect.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <csignal>
#include <csetjmp>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu_stm_api.h"

#include "tm_hooks.hpp"
#include "tm_region_allocator.hpp"
#include "tm_thread_state.hpp"

// ── Global state ───────────────────────────────────────────────────

// Device-memory pointers (extern, set by init_device_memory)
uint32_t *d_lock_table = nullptr;
uint64_t *d_global_clock = nullptr;
uint32_t *d_data = nullptr;
int       gpu_num_addresses = 1024;
uint64_t *d_committed = nullptr;
uint64_t *d_aborted = nullptr;

// Host-side mirror for CPU fallback
static uint32_t *h_lock_table = nullptr;
static uint32_t *h_data = nullptr;

static std::mutex gpu_init_mutex;
static thread_local int tl_in_tx = 0;

// Plugin mode: extern TLS variables
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// ── Device management ──────────────────────────────────────────────

static int check_gpu_available() {
    return gpu_check_and_set(0);
}

static void init_device_memory(int num_addrs) {
    CUDA_CHECK(cudaMalloc(&d_lock_table,
               PR_STM_LOCKTABLE_SIZE * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemset(d_lock_table, 0,
               PR_STM_LOCKTABLE_SIZE * sizeof(uint32_t)));

    CUDA_CHECK(cudaMalloc(&d_global_clock, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_global_clock, 0, sizeof(uint64_t)));

    CUDA_CHECK(cudaMalloc(&d_data, num_addrs * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemset(d_data, 0, num_addrs * sizeof(uint32_t)));

    CUDA_CHECK(cudaMalloc(&d_committed, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_committed, 0, sizeof(uint64_t)));

    CUDA_CHECK(cudaMalloc(&d_aborted, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_aborted, 0, sizeof(uint64_t)));
}

static void init_host_memory(int num_addrs) {
    h_lock_table = (uint32_t *)calloc(
        PR_STM_LOCKTABLE_SIZE, sizeof(uint32_t));
    h_data = (uint32_t *)calloc(num_addrs, sizeof(uint32_t));
}

// ── Host API implementations (TMRealHooks) ─────────────────────────

void gpu_tm_init() {
    std::lock_guard<std::mutex> lock(gpu_init_mutex);

    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed\n");
        abort();
    }

    g_gpu_available = check_gpu_available();
    gpu_num_addresses = 1024;

    if (g_gpu_available) {
        cudaSetDevice(0);
        init_device_memory(gpu_num_addresses);
        printf("[PR-STM] GPU available. Device memory initialized.\n");
    } else {
        init_host_memory(gpu_num_addresses);
        printf("[PR-STM] No GPU found. Using CPU fallback.\n");
    }

    extern const TMRealHooks g_pr_stm_hooks;
    tm_register_real_hooks(&g_pr_stm_hooks);
}

void gpu_tm_exit() {
    if (g_gpu_available) {
        cudaFree(d_lock_table);
        cudaFree(d_global_clock);
        cudaFree(d_data);
        cudaFree(d_committed);
        cudaFree(d_aborted);
        cudaDeviceReset();
    } else {
        free(h_lock_table);
        free(h_data);
    }
}

void gpu_tm_init_thread() {
    tm_hook_init_thread();
}

void gpu_tm_exit_thread() {
    tm_hook_exit_thread();
}

void gpu_tm_begin() {
    tl_in_tx = 1;
}

void gpu_tm_end() {
    tl_in_tx = 0;
}

void *gpu_tm_malloc(size_t sz) {
    void *p;
    if (g_gpu_available) {
        CUDA_CHECK(cudaMallocManaged(&p, sz));
    } else {
        p = stm::tm_region_malloc(sz);
    }
    if (p) memset(p, 0, sz);
    return p;
}

void gpu_tm_free(void *p) {
    if (!p) return;
    if (g_gpu_available) {
        cudaFree(p);
    } else {
        stm::tm_region_free(p);
    }
}

void *gpu_tm_calloc(size_t nmemb, size_t sz) {
    size_t total = nmemb * sz;
    void *p = gpu_tm_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *gpu_tm_realloc(void *p, size_t sz) {
    if (!p) return gpu_tm_malloc(sz);
    void *n = gpu_tm_malloc(sz);
    if (n && sz > 0) {
        memcpy(n, p, sz);
        gpu_tm_free(p);
    }
    return n;
}

uint8_t  gpu_tm_read_i1(uint8_t *addr)  { return *addr; }
uint16_t gpu_tm_read_i2(uint16_t *addr) { return *addr; }
uint32_t gpu_tm_read_i4(uint32_t *addr) { return *addr; }
uint64_t gpu_tm_read_i8(uint64_t *addr) { return *addr; }
float    gpu_tm_read_f4(float *addr)    { return *addr; }
double   gpu_tm_read_f8(double *addr)   { return *addr; }
void    *gpu_tm_read_ptr(void **addr)   { return *addr; }

void gpu_tm_write_i1(uint8_t *addr, uint8_t val)  { *addr = val; }
void gpu_tm_write_i2(uint16_t *addr, uint16_t val) { *addr = val; }
void gpu_tm_write_i4(uint32_t *addr, uint32_t val) { *addr = val; }
void gpu_tm_write_i8(uint64_t *addr, int64_t val)  { *addr = val; }
void gpu_tm_write_f4(float *addr, float val)       { *addr = val; }
void gpu_tm_write_f8(double *addr, double val)     { *addr = val; }
void gpu_tm_write_ptr(void **addr, void *val)      { *addr = val; }

static void *real_tm_get_thread_state() {
    thread_local static TMThreadState state{0, 0};
    return (void*)&state;
}

const TMRealHooks g_pr_stm_hooks = {
    .begin    = gpu_tm_begin,
    .end      = gpu_tm_end,
    .malloc   = gpu_tm_malloc,
    .calloc   = gpu_tm_calloc,
    .realloc  = gpu_tm_realloc,
    .free     = gpu_tm_free,
    .read_i1  = gpu_tm_read_i1,
    .read_i2  = gpu_tm_read_i2,
    .read_i4  = gpu_tm_read_i4,
    .read_i8  = gpu_tm_read_i8,
    .read_f4  = gpu_tm_read_f4,
    .read_f8  = gpu_tm_read_f8,
    .read_ptr = gpu_tm_read_ptr,
    .write_i1  = gpu_tm_write_i1,
    .write_i2  = gpu_tm_write_i2,
    .write_i4  = gpu_tm_write_i4,
    .write_i8  = gpu_tm_write_i8,
    .write_f4  = gpu_tm_write_f4,
    .write_f8  = gpu_tm_write_f8,
    .write_ptr = gpu_tm_write_ptr,
    .get_thread_state = real_tm_get_thread_state,
};

// ── CPU PR-STM emulation (std::thread-based) ───────────────────────

struct CpuLaneState {
    int warp_id;
    int lane_id;
    uint32_t read_addrs[PR_STM_MAX_READS];
    uint32_t read_vers[PR_STM_MAX_READS];
    int num_reads;
    uint32_t write_addrs[PR_STM_MAX_WRITES];
    int num_writes;
    uint8_t priority;
    volatile int phase;
};

static std::mutex cpu_pr_stm_mutex;

void cpu_pr_stm_begin(int warp_id, int lane_id) {
    (void)warp_id; (void)lane_id;
}

void cpu_pr_stm_read(int warp_id, int lane_id, uint32_t *addr) {
    (void)warp_id; (void)lane_id; (void)addr;
}

void cpu_pr_stm_write(int warp_id, int lane_id, uint32_t *addr, uint32_t val) {
    (void)warp_id; (void)lane_id; (void)addr; (void)val;
}

int cpu_pr_stm_commit(int warp_id, int lane_id) {
    (void)warp_id; (void)lane_id;
    return 1;
}

void cpu_pr_stm_abort(int warp_id, int lane_id) {
    (void)warp_id; (void)lane_id;
}

void cpu_pr_stm_end(int warp_id, int lane_id) {
    (void)warp_id; (void)lane_id;
}
