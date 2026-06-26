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
#include <unistd.h>
#include <unordered_set>

#include "tl2.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_backend_macros.hpp"
#include "tm_thread_state.hpp"
#include "tm_hooks.hpp"
// TL2-specific jmpbuf tracking (defined here, declared extern in tl2.hpp)
thread_local bool tm_jmpbuf_initialized = false;

// Thread-local state
static __thread int8_t tm_is_init_ready = 0;
static __thread TMThreadState g_tm_thread_state = {0, 0};

static void *real_tm_get_thread_state() {
    return (void*)&g_tm_thread_state;
}

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_commit_fail_count{0};
std::atomic<uint64_t> g_tm_abort_count{0};

extern const TMRealHooks g_tl2_hooks;

#ifdef LLVM_TM_PLUGIN
static void do_tm_init();
static void do_tm_exit();
static void do_tm_init_thread();
static void do_tm_exit_thread();

void (*tm_init)()        = do_tm_init;
void (*tm_exit)()        = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;

static void do_tm_init()
#else
extern "C" void tm_init()
#endif
{
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed — TM address space unavailable\n");
        std::abort();
    }
    tl2::init();
    tm_register_real_hooks(&g_tl2_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
extern "C" void tm_exit()
#endif
{
    stm::tm_region_destroy();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
extern "C" void tm_init_thread()
#endif
{
    tm_hook_init_thread();
    tl2::init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
extern "C" void tm_exit_thread()
#endif
{
    tm_hook_exit_thread();
    tl2::exit_thread();
}





// Wrapper functions matching plugin interface (2-arg read/write, no symbol_id)

static void real_tm_begin() {
    tl2::set_jmp_env_external(&tm_jmpbuf);
    tm_clear_spec_allocs();
    tm_clear_deferred_frees();
    g_in_tx = true;
    tl2::begin();
    assert(tm_nested_call_counter >= 0);
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

static void real_tm_end() {
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
static uint8_t real_tm_read_i1(uint8_t *addr) {
    return tl2::tm_read_i1(addr);
}

static uint16_t real_tm_read_i2(uint16_t *addr) {
    return tl2::tm_read_i2(addr);
}

static uint32_t real_tm_read_i4(uint32_t *addr) {
    uint32_t r = tl2::tm_read_i4(addr);
    return r;
}

static uint64_t real_tm_read_i8(uint64_t *addr) {
    return tl2::tm_read_i8(addr);
}

static float real_tm_read_f4(float *addr) {
    return tl2::tm_read_f4(addr);
}

static double real_tm_read_f8(double *addr) {
    return tl2::tm_read_f8(addr);
}

static void *real_tm_read_ptr(void **addr) {
    return tl2::tm_read_ptr((volatile void**)addr);
}

// Write wrappers — 2 args (addr + val, symbol_id removed)
static void real_tm_write_i1(uint8_t *addr, uint8_t val) {
    tl2::tm_write_i1(addr, val);
}

static void real_tm_write_i2(uint16_t *addr, uint16_t val) {
    tl2::tm_write_i2(addr, val);
}

static void real_tm_write_i4(uint32_t *addr, uint32_t val) {
    tl2::tm_write_i4(addr, val);
}

static void real_tm_write_i8(uint64_t *addr, int64_t val) {
    tl2::tm_write_i8(addr, val);
}

static void real_tm_write_f4(float *addr, float val) {
    tl2::tm_write_f4(addr, val);
}

static void real_tm_write_f8(double *addr, double val) {
    tl2::tm_write_f8(addr, val);
}

static void real_tm_write_ptr(void **addr, void *val) {
    tl2::tm_write_ptr((volatile void**)addr, val);
}

TM_DEFINE_PLUGIN_RW(tl2)

extern "C" void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {
}

static void* real_tm_malloc(size_t size) { return tm_track_alloc_result(stm::tm_region_malloc(size), size); }
static void* real_tm_calloc(size_t nmemb, size_t size) { void* p = stm::tm_region_malloc(nmemb * size); memset(p, 0, nmemb * size); return tm_track_alloc_result(p, nmemb * size); }
static void* real_tm_realloc(void* ptr, size_t size) { void* p = stm::tm_region_malloc(size); if (ptr) { memcpy(p, ptr, size); stm::tm_region_free(ptr); } return tm_track_alloc_result(p, size); }
static void  real_tm_free(void* ptr) {
    if (!ptr || !stm::isTMAddress(ptr)) return;
    TM_EVENT(FREE, ptr, 0);
    if (g_in_tx) {
        tl2::tm_write_i1(reinterpret_cast<volatile uint8_t*>(ptr), 0);
        tm_free_append_deferred(ptr);
    } else {
        stm::tm_region_free(ptr);
    }
}

TM_REAL_HOOKS_TABLE(tl2)
