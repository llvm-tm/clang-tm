#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_set>

#include "leftright.hpp"
#include "../common/tm_alloc_overrides.hpp"
#include "../common/tm_thread_state.hpp"
#include "../common/tm_hooks.hpp"

thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;

__thread sigjmp_buf *jmpbuf;

namespace leftright {

std::atomic<uint64_t> g_clock{1};
std::atomic<uint64_t> thr_counter{1};
__thread Transaction *current_tx = nullptr;
std::atomic<uint64_t> g_tm_abort_count{0};

std::atomic<uint64_t> g_left_barrier{0};
std::atomic<uint64_t> g_right_barrier{0};
std::atomic<uint64_t> g_left_phase{0};
std::atomic<uint64_t> g_right_phase{0};

__thread sigjmp_buf *jmpbuf_ptr;

} // namespace leftright

// Separate thread-local counters:
//   tm_nested_call_counter  — managed by tm_begin/tm_end (explicit API calls)
//   g_tm_thread_state       — managed by generated plugin clone code
// The clone writes to g_tm_thread_state.nested_call_counter via
// tm_get_thread_state() + GEP, then calls tm_begin().  tm_begin()
// reads both to determine if this is plugin-mode or explicit-mode entry.
static __thread TMThreadState g_tm_thread_state = {0, 0};
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;

__thread sigjmp_buf tm_jmpbuf;
__thread int tm_init_thread_call_count = 0;

// Define the global queue-active flag locally (queue_runtime.cpp is not linked).
std::atomic<int> g_tm_queue_global{0};

extern const TMRealHooks g_leftright_hooks;

extern "C" {

void tm_init() {
    leftright::init();
    tm_register_real_hooks(&g_leftright_hooks);
}

void tm_exit() {
    leftright::exit();
    if (auto ac = leftright::g_tm_abort_count.load(); ac > 0) {
        fprintf(stderr, "\n=== LeftRight total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

void tm_init_thread() {
    leftright::init_thread();
}

void tm_exit_thread() {}

TMThreadState *tm_get_thread_state() {
    return &g_tm_thread_state;
}

void tm_set_jmpbuf(void *buf) {
    leftright::jmpbuf_ptr = (sigjmp_buf *)buf;
}

sigjmp_buf *tm_get_env() {
    return &tm_jmpbuf;
}

} // extern "C" — non-hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

// Forward declarations for static hook functions
static void real_tm_free(void *ptr);

static void real_tm_begin() {
    int32_t tc = tm_nested_call_counter;
    int32_t sc = g_tm_thread_state.nested_call_counter;
    int32_t c = (tc > 0) ? tc : sc;
    if (c <= 1)
        leftright::begin();
}

static void real_tm_end() {
    int32_t tc = tm_nested_call_counter;
    int32_t sc = g_tm_thread_state.nested_call_counter;
    int32_t c = (tc > 0) ? tc : sc;
    if (c <= 1)
        leftright::commit();
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

static void real_tm_free(void *ptr) {
    if (!ptr) return;
    tm_untrack_spec_alloc(ptr);
    if (g_in_tx)
        tm_free_append_deferred(ptr);
    else
        ::operator delete(ptr);
}

static uint8_t real_tm_read_i1(uint8_t *addr) { return leftright::tm_read<uint8_t, leftright::ValueType::UINT8>(addr); }
static uint16_t real_tm_read_i2(uint16_t *addr) { return leftright::tm_read<uint16_t, leftright::ValueType::UINT16>(addr); }
static uint32_t real_tm_read_i4(uint32_t *addr) { return leftright::tm_read<uint32_t, leftright::ValueType::UINT32>(addr); }
static uint64_t real_tm_read_i8(uint64_t *addr) { return leftright::tm_read<uint64_t, leftright::ValueType::UINT64>(addr); }
static void real_tm_write_i1(uint8_t *addr, uint8_t val) { leftright::tm_write<uint8_t, leftright::ValueType::UINT8>(addr, val); }
static void real_tm_write_i2(uint16_t *addr, uint16_t val) { leftright::tm_write<uint16_t, leftright::ValueType::UINT16>(addr, val); }
static void real_tm_write_i4(uint32_t *addr, uint32_t val) { leftright::tm_write<uint32_t, leftright::ValueType::UINT32>(addr, val); }
static void real_tm_write_i8(uint64_t *addr, int64_t val) { leftright::tm_write<uint64_t, leftright::ValueType::UINT64>(addr, static_cast<uint64_t>(val)); }

static float real_tm_read_f4(float *addr) { return leftright::tm_read<float, leftright::ValueType::FLOAT>(addr); }
static double real_tm_read_f8(double *addr) { return leftright::tm_read<double, leftright::ValueType::DOUBLE>(addr); }
static void real_tm_write_f4(float *addr, float val) { leftright::tm_write<float, leftright::ValueType::FLOAT>(addr, val); }
static void real_tm_write_f8(double *addr, double val) { leftright::tm_write<double, leftright::ValueType::DOUBLE>(addr, val); }

static void *real_tm_read_ptr(void **addr) { return leftright::tm_read<void *, leftright::ValueType::POINTER>(addr); }
static void real_tm_write_ptr(void **addr, void *val) { leftright::tm_write<void *, leftright::ValueType::POINTER>(addr, val); }

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

const TMRealHooks g_leftright_hooks = {
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
