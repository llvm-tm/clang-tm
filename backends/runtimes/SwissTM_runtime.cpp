/**
 * SwissTM Runtime Wrapper for LLVM TM Plugin
 * Uses SwissTM for transactional memory
 */

#include <cstdint>
#include <csetjmp>
#include <cassert>
#include <cstdio>
#include <atomic>
#include <cstring>

#include "../SwissTM/SwissTM.hpp"

// Thread-local state
static __thread int8_t tm_is_init_ready = 0;
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_jmpbuf_ret = 0;
static __thread sigjmp_buf tm_jmpbuf;

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};

extern "C" void tm_init() {
    fprintf(stderr, "tm_init called\n");
    swisstm::init();
}

extern "C" void tm_exit() {
}

extern "C" void tm_init_thread() {
    fprintf(stderr, "tm_init_thread called\n");
    swisstm::init_thread();
}

extern "C" void tm_exit_thread() {
    swisstm::exit_thread();
}

extern "C" int tm_setjmp() {
    return 0;
}

extern "C" sigjmp_buf* tm_get_env() {
    return &tm_jmpbuf;
}

extern "C" void tm_set_env(sigjmp_buf* env) {
    if (env) {
        memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
        tm_is_init_ready = 1;
    }
}

// Wrapper functions matching plugin interface

extern "C" void tm_begin() {
    swisstm::begin();
    fprintf(stderr, "tm_begin: active=%d\n", swisstm::active());
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
    if (g_tm_begin_count.load() % 100000 == 0) {
        fprintf(stderr, "tm_begin count: %lld\n", (long long)g_tm_begin_count.load());
    }
}

extern "C" void tm_end() {
    fprintf(stderr, "tm_end called\n");
    fflush(stderr);
    bool result = swisstm::commit();
    g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
    if (!result) {
        fprintf(stderr, "tm_end: COMMIT FAILED\n");
        fflush(stderr);
    }
}

// Read wrappers with symbol_id
extern "C" uint8_t tm_read_i1(uint8_t *addr, uint32_t symbol_id) {
    return swisstm::tm_read_i1(addr);
}

extern "C" uint16_t tm_read_i2(uint16_t *addr, uint32_t symbol_id) {
    return swisstm::tm_read_i2(addr);
}

extern "C" uint32_t tm_read_i4(uint32_t *addr, uint32_t symbol_id) {
    return swisstm::tm_read_i4(addr);
}

extern "C" uint64_t tm_read_i8(uint64_t *addr, uint32_t symbol_id) {
    return swisstm::tm_read_i8(addr);
}

extern "C" float tm_read_f4(float *addr, uint32_t symbol_id) {
    return swisstm::tm_read_f4(addr);
}

extern "C" double tm_read_f8(double *addr, uint32_t symbol_id) {
    return swisstm::tm_read_f8(addr);
}

extern "C" void *tm_read_ptr(void **addr, uint32_t symbol_id) {
    return swisstm::tm_read_ptr(addr);
}

extern "C" void *tm_read_z(uint8_t *addr, uint64_t len, uint32_t symbol_id) {
    assert(len < TM_BUFFER_SIZE);
    for (uint64_t i = 0; i < len; i++) {
        tm_buffer[i] = swisstm::tm_read_i1(&addr[i]);
    }
    return tm_buffer;
}

// Write wrappers with symbol_id
extern "C" void tm_write_i1(uint8_t *addr, uint8_t val, uint32_t symbol_id) {
    swisstm::tm_write_i1(addr, val);
}

extern "C" void tm_write_i2(uint16_t *addr, uint16_t val, uint32_t symbol_id) {
    swisstm::tm_write_i2(addr, val);
}

extern "C" void tm_write_i4(uint32_t *addr, uint32_t val, uint32_t symbol_id) {
    swisstm::tm_write_i4(addr, val);
}

extern "C" void tm_write_i8(uint64_t *addr, uint64_t val, uint32_t symbol_id) {
    swisstm::tm_write_i8(addr, val);
}

extern "C" void tm_write_f4(float *addr, float val, uint32_t symbol_id) {
    swisstm::tm_write_f4(addr, val);
}

extern "C" void tm_write_f8(double *addr, double val, uint32_t symbol_id) {
    swisstm::tm_write_f8(addr, val);
}

extern "C" void tm_write_ptr(void **addr, void *val, uint32_t symbol_id) {
    swisstm::tm_write_ptr(addr, val);
}

extern "C" void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len, uint32_t symbol_id) {
    for (uint64_t i = 0; i < len; i++) {
        swisstm::tm_write_i1(&dst[i], src[i]);
    }
}

extern "C" void tm_memset(uint8_t *addr, uint8_t val, uint64_t len, uint32_t symbol_id) {
    for (uint64_t i = 0; i < len; i++) {
        swisstm::tm_write_i1(&addr[i], val);
    }
}

extern "C" void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {
}

static void print_stats() {
    fprintf(stderr, "=== SwissTM_new Runtime Stats ===\n");
    fprintf(stderr, "tm_begin: %lld, tm_end: %lld\n", 
        (long long)g_tm_begin_count.load(std::memory_order_relaxed), 
        (long long)g_tm_end_count.load(std::memory_order_relaxed));
}

static int init = (std::atexit(print_stats), 0);