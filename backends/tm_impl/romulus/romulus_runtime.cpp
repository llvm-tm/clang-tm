#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_set>

#include "romulus.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_hooks.hpp"
extern const TMRealHooks g_romulus_hooks;

thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;

namespace romulus {

std::atomic<uint64_t> g_global_clock{1};
std::atomic<uint64_t> thr_counter{1};
__thread Transaction *current_tx = nullptr;
std::atomic<uint64_t> g_tm_abort_count{0};
__thread sigjmp_buf *jmpbuf_ptr;

} // namespace romulus

__thread sigjmp_buf *jmpbuf;

__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
__thread sigjmp_buf tm_jmpbuf;
__thread int tm_init_thread_call_count = 0;

extern "C" {

void tm_init() {
    romulus::init();
    tm_register_real_hooks(&g_romulus_hooks);
}

void tm_exit() {
    romulus::exit();
    if (auto ac = romulus::g_tm_abort_count.load(); ac > 0) {
        fprintf(stderr, "\n=== Romulus total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

void tm_init_thread() { tm_hook_init_thread(); romulus::init_thread(); }

void tm_exit_thread() { tm_hook_exit_thread(); }

} // extern "C" — non-hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void real_tm_begin() {
    if (tm_nested_call_counter == 0) {
        tm_nested_call_counter = 1;
    } else {
        tm_nested_call_counter++;
        return;
    }
    romulus::jmpbuf_ptr = (sigjmp_buf *)&tm_jmpbuf;
    romulus::begin();
}

static void real_tm_end() {
    if (tm_nested_call_counter == 1) {
        romulus::commit();
        tm_nested_call_counter = 0;
    } else if (tm_nested_call_counter > 1) {
        tm_nested_call_counter--;
    }
}

static void *real_tm_malloc(size_t size) {
    void *p = ::operator new(size);
    if (p) tm_track_spec_alloc(p);
    return p;
}

static void *real_tm_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = ::operator new(total);
    if (p) { std::memset(p, 0, total); tm_track_spec_alloc(p); }
    return p;
}

static void real_tm_free(void *ptr) {
    if (!ptr) return;
    tm_untrack_spec_alloc(ptr);
    if (g_in_tx)
        tm_free_append_deferred(ptr);
    else
        ::operator delete(ptr);
}

static void *real_tm_realloc(void *ptr, size_t size) {
    if (!ptr) return real_tm_malloc(size);
    void *p = ::operator new(size);
    if (p) {
        std::memcpy(p, ptr, size);
        real_tm_free(ptr);
        tm_track_spec_alloc(p);
    }
    return p;
}

static uint8_t  real_tm_read_i1(uint8_t *addr)  { return romulus::tm_read<uint8_t, romulus::ValueType::UINT8>(addr); }
static uint16_t real_tm_read_i2(uint16_t *addr) { return romulus::tm_read<uint16_t, romulus::ValueType::UINT16>(addr); }
static uint32_t real_tm_read_i4(uint32_t *addr) { return romulus::tm_read<uint32_t, romulus::ValueType::UINT32>(addr); }
static uint64_t real_tm_read_i8(uint64_t *addr) { return romulus::tm_read<uint64_t, romulus::ValueType::UINT64>(addr); }
static void    real_tm_write_i1(uint8_t *addr, uint8_t val)   { romulus::tm_write<uint8_t, romulus::ValueType::UINT8>(addr, val); }
static void    real_tm_write_i2(uint16_t *addr, uint16_t val) { romulus::tm_write<uint16_t, romulus::ValueType::UINT16>(addr, val); }
static void    real_tm_write_i4(uint32_t *addr, uint32_t val) { romulus::tm_write<uint32_t, romulus::ValueType::UINT32>(addr, val); }
static void    real_tm_write_i8(uint64_t *addr, int64_t val) { romulus::tm_write<uint64_t, romulus::ValueType::UINT64>(addr, (uint64_t)val); }

static float  real_tm_read_f4(float *addr)  { return romulus::tm_read<float, romulus::ValueType::FLOAT>(addr); }
static double real_tm_read_f8(double *addr) { return romulus::tm_read<double, romulus::ValueType::DOUBLE>(addr); }
static void   real_tm_write_f4(float *addr, float val)     { romulus::tm_write<float, romulus::ValueType::FLOAT>(addr, val); }
static void   real_tm_write_f8(double *addr, double val)   { romulus::tm_write<double, romulus::ValueType::DOUBLE>(addr, val); }

static void *real_tm_read_ptr(void **addr)  { return romulus::tm_read<void *, romulus::ValueType::POINTER>(addr); }
static void  real_tm_write_ptr(void **addr, void *val) { romulus::tm_write<void *, romulus::ValueType::POINTER>(addr, val); }

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

const TMRealHooks g_romulus_hooks = {
    .begin    = real_tm_begin,
    .end      = real_tm_end,
    .malloc   = real_tm_malloc,
    .calloc   = real_tm_calloc,
    .realloc  = real_tm_realloc,
    .free     = real_tm_free,
    .read_i1  = real_tm_read_i1,
    .read_i2  = real_tm_read_i2,
    .read_i4  = real_tm_read_i4,
    .read_i8  = real_tm_read_i8,
    .read_f4  = real_tm_read_f4,
    .read_f8  = real_tm_read_f8,
    .read_ptr = real_tm_read_ptr,
    .write_i1  = real_tm_write_i1,
    .write_i2  = real_tm_write_i2,
    .write_i4  = real_tm_write_i4,
    .write_i8  = real_tm_write_i8,
    .write_f4  = real_tm_write_f4,
    .write_f8  = real_tm_write_f8,
    .write_ptr = real_tm_write_ptr,
};
