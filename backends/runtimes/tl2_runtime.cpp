/**
 * TL2 Runtime Wrapper for LLVM TM Plugin
 * Uses TL2 for transactional memory
 */

#include <cstdint>
#include <csetjmp>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <mutex>

#include "../TL2/tl2.hpp"
#include "../tm_alloc_overrides.hpp"
thread_local bool g_in_tx = false;
thread_local FreeNode* g_deferred_frees = nullptr;
thread_local SpecAlloc* g_spec_allocs = nullptr;

// Thread-local state
static __thread int8_t tm_is_init_ready = 0;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
__thread sigjmp_buf tm_jmpbuf;

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_commit_fail_count{0};
static std::atomic<int64_t> g_tm_abort_count{0};

extern "C" void tm_init() {
    tl2::init();
}

extern "C" void tm_exit() {
    // Cleanup - nothing special needed for simplified version
}

extern "C" void tm_init_thread() {
    tl2::init_thread();
}

extern "C" void tm_exit_thread() {
    tl2::exit_thread();
}

static std::recursive_mutex g_serialize_mutex;

extern "C" void tm_serialize_lock() { g_serialize_mutex.lock(); }

extern "C" void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

extern "C" int tm_setjmp() {
    return 0;
}

extern "C" void tm_set_jmpbuf(void *buf) { }

extern "C" sigjmp_buf* tm_get_env() {
    return &tm_jmpbuf;
}

extern "C" void tm_set_env(sigjmp_buf* env) {
    if (env) {
        memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
        tm_is_init_ready = 1;
    }
}

// Wrapper functions matching plugin interface (2-arg read/write, no symbol_id)

extern "C" void tm_begin() {
    //fprintf(stderr, "TL2 tm_begin called (thr=%ld)\n", (long)pthread_self());
    tl2::set_jmp_env_external(&tm_jmpbuf);
    tm_clear_spec_allocs();
    tm_clear_deferred_frees();
    g_in_tx = true;
    tl2::begin();
    assert(tm_nested_call_counter >= 0);
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void tm_end() {
    //fprintf(stderr, "TL2 tm_end called (thr=%ld)\n", (long)pthread_self());
    if (!tl2::commit()) {
        g_tm_commit_fail_count.fetch_add(1, std::memory_order_relaxed);
        siglongjmp(tm_jmpbuf, 1);
        __builtin_unreachable();
    }
    g_in_tx = false;
    tm_flush_spec_allocs();
    tm_flush_deferred_frees();
    assert(tm_nested_call_counter >= 0);
    g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
}

// Read wrappers — 1 arg (addr only, symbol_id removed)
extern "C" uint8_t tm_read_i1(uint8_t *addr) {
    return tl2::tm_read_i1(addr);
}

extern "C" uint16_t tm_read_i2(uint16_t *addr) {
    return tl2::tm_read_i2(addr);
}

extern "C" uint32_t tm_read_i4(uint32_t *addr) {
    uint32_t r = tl2::tm_read_i4(addr);
    return r;
}

extern "C" uint64_t tm_read_i8(uint64_t *addr) {
    return tl2::tm_read_i8(addr);
}

extern "C" float tm_read_f4(float *addr) {
    return tl2::tm_read_f4(addr);
}

extern "C" double tm_read_f8(double *addr) {
    return tl2::tm_read_f8(addr);
}

extern "C" void *tm_read_ptr(void **addr) {
    return tl2::tm_read_ptr((volatile void**)addr);
}

extern "C" void *tm_read_z(uint8_t *addr, uint64_t len) {
    assert(len < TM_BUFFER_SIZE);
    for (uint64_t i = 0; i < len; i++) {
        tm_buffer[i] = tl2::tm_read_i1(&addr[i]);
    }
    return tm_buffer;
}

// Write wrappers — 2 args (addr + val, symbol_id removed)
extern "C" void tm_write_i1(uint8_t *addr, uint8_t val) {
    tl2::tm_write_i1(addr, val);
}

extern "C" void tm_write_i2(uint16_t *addr, uint16_t val) {
    tl2::tm_write_i2(addr, val);
}

extern "C" void tm_write_i4(uint32_t *addr, uint32_t val) {
    tl2::tm_write_i4(addr, val);
}

extern "C" void tm_write_i8(uint64_t *addr, uint64_t val) {
    tl2::tm_write_i8(addr, val);
}

extern "C" void tm_write_f4(float *addr, float val) {
    tl2::tm_write_f4(addr, val);
}

extern "C" void tm_write_f8(double *addr, double val) {
    tl2::tm_write_f8(addr, val);
}

extern "C" void tm_write_ptr(void **addr, void *val) {
    tl2::tm_write_ptr((volatile void**)addr, val);
}

extern "C" void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        tl2::tm_write_i1(&dst[i], src[i]);
    }
}

extern "C" void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {
}

extern "C" void* tm_malloc(size_t size) { void* p = malloc(size); tm_track_spec_alloc(p); return p; }
extern "C" void* tm_calloc(size_t nmemb, size_t size) { void* p = calloc(nmemb, size); tm_track_spec_alloc(p); return p; }
extern "C" void* tm_realloc(void* ptr, size_t size) { void* p = realloc(ptr, size); tm_track_spec_alloc(p); return p; }
extern "C" void  tm_free(void* ptr) {
    if (g_in_tx) {
        tm_untrack_spec_alloc(ptr);
        auto* node = static_cast<FreeNode*>(::malloc(sizeof(FreeNode)));
        node->ptr = ptr;
        node->next = g_deferred_frees;
        g_deferred_frees = node;
    } else {
        ::free(ptr);
    }
}

static void print_stats() {
#ifndef NDEBUG
    fprintf(stderr, "=== TL2 Runtime Stats ===\n");
    fprintf(stderr, "tm_begin: %lld, tm_end: %lld, commit_fail: %lld, abort_count: %lld\n", 
        (long long)g_tm_begin_count.load(std::memory_order_relaxed), 
        (long long)g_tm_end_count.load(std::memory_order_relaxed),
        (long long)g_tm_commit_fail_count.load(std::memory_order_relaxed),
        (long long)g_tm_abort_count.load(std::memory_order_relaxed));
#endif
}

static int init = (std::atexit(print_stats), 0);
