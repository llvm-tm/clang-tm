#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_set>

#include "xtm.hpp"
#include "../common/tm_alloc_overrides.hpp"
#include "tm_hooks.hpp"

thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;

namespace xtm {

std::atomic<uint64_t> g_tx_counter{1};
std::atomic<uint64_t> g_abort_counter{0};
XADTEntry *g_xadt = nullptr;
std::atomic<uint8_t> g_xf[XF_BITS];
__thread Transaction *current_tx = nullptr;
__thread sigjmp_buf *jmpbuf_ptr = nullptr;

} // namespace xtm

__thread sigjmp_buf *jmpbuf = nullptr;
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
__thread int tm_init_thread_call_count = 0;

extern const TMRealHooks g_xtm_hooks;

extern "C" void tm_init() {
    stm::tm_region_init();
    xtm::init();
    tm_register_real_hooks(&g_xtm_hooks);
}

extern "C" void tm_exit() {
    xtm::exit();
    if (auto ac = xtm::g_abort_counter.load(); ac > 0) {
        fprintf(stderr, "\n=== XTM total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

extern "C" void tm_init_thread() {
    tm_hook_init_thread();
    xtm::init_thread();
}

extern "C" void tm_exit_thread() { tm_hook_exit_thread(); }

static void real_tm_begin() {
    if (tm_nested_call_counter == 1) {
        xtm::jmpbuf_ptr = (sigjmp_buf *)&tm_jmpbuf;
        xtm::begin();
    }
}

static void real_tm_end() {
    if (tm_nested_call_counter == 1) {
        xtm::commit();
    }
}

static void *real_tm_malloc(size_t size) {
    void *p = stm::tm_region_malloc(size);
    if (p) tm_track_spec_alloc(p);
    return p;
}

static void *real_tm_calloc(size_t nmemb, size_t size) {
    void *p = stm::tm_region_calloc(nmemb, size);
    if (p) tm_track_spec_alloc(p);
    return p;
}

static void *real_tm_realloc(void *ptr, size_t size) {
    return stm::tm_region_realloc(ptr, size);
}

static void real_tm_free(void *ptr) {
    if (!ptr) return;
    tm_untrack_spec_alloc(ptr);
    stm::tm_region_free(ptr);
}

static uint8_t  real_tm_read_i1(uint8_t  *addr) { return xtm::tm_read<uint8_t,  xtm::ValueType::UINT8>(addr); }
static uint16_t real_tm_read_i2(uint16_t *addr) { return xtm::tm_read<uint16_t, xtm::ValueType::UINT16>(addr); }
static uint32_t real_tm_read_i4(uint32_t *addr) { return xtm::tm_read<uint32_t, xtm::ValueType::UINT32>(addr); }
static uint64_t real_tm_read_i8(uint64_t *addr) { return xtm::tm_read<uint64_t, xtm::ValueType::UINT64>(addr); }

static void real_tm_write_i1(uint8_t  *addr, uint8_t  val) { xtm::tm_write<uint8_t,  xtm::ValueType::UINT8>(addr, val); }
static void real_tm_write_i2(uint16_t *addr, uint16_t val) { xtm::tm_write<uint16_t, xtm::ValueType::UINT16>(addr, val); }
static void real_tm_write_i4(uint32_t *addr, uint32_t val) { xtm::tm_write<uint32_t, xtm::ValueType::UINT32>(addr, val); }
static void real_tm_write_i8(uint64_t *addr, int64_t val) { xtm::tm_write<uint64_t, xtm::ValueType::UINT64>(addr, val); }

static float  real_tm_read_f4(float  *addr) { return xtm::tm_read<float,  xtm::ValueType::FLOAT>(addr); }
static double real_tm_read_f8(double *addr) { return xtm::tm_read<double, xtm::ValueType::DOUBLE>(addr); }

static void real_tm_write_f4(float  *addr, float  val) { xtm::tm_write<float,  xtm::ValueType::FLOAT>(addr, val); }
static void real_tm_write_f8(double *addr, double val) { xtm::tm_write<double, xtm::ValueType::DOUBLE>(addr, val); }

static void *real_tm_read_ptr(void **addr) { return xtm::tm_read<void *, xtm::ValueType::POINTER>(addr); }
static void  real_tm_write_ptr(void **addr, void *val) { xtm::tm_write<void *, xtm::ValueType::POINTER>(addr, val); }

const TMRealHooks g_xtm_hooks = {
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
