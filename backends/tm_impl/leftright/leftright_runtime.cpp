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

namespace leftright {

std::atomic<uint64_t> g_clock{1};
std::atomic<uint64_t> thr_counter{1};
__thread Transaction *current_tx = nullptr;
std::atomic<uint64_t> g_tm_abort_count{0};

std::atomic<uint32_t> g_commit_lock{0};

__thread sigjmp_buf *jmpbuf_ptr;

} // namespace leftright

// Separate thread-local counters:
//   tm_nested_call_counter  — managed by tm_begin/tm_end (explicit API calls)
//   g_tm_thread_state       — managed by generated plugin clone code
// The clone writes to g_tm_thread_state.nested_call_counter via
// tm_get_thread_state() + GEP, then calls tm_begin().  tm_begin()
// reads both to determine if this is plugin-mode or explicit-mode entry.
static __thread TMThreadState g_tm_thread_state = {0, 0};
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
__thread int tm_init_thread_call_count = 0;

// Define the global queue-active flag locally (queue_runtime.cpp is not linked).
std::atomic<int> g_tm_queue_global{0};

extern const TMRealHooks g_leftright_hooks;

extern "C" {

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
void tm_init()
#endif
{
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed\n");
        abort();
    }
    leftright::init();
    tm_register_real_hooks(&g_leftright_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    leftright::exit();
    if (auto ac = leftright::g_tm_abort_count.load(); ac > 0) {
        fprintf(stderr, "\n=== LeftRight total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
    tm_hook_init_thread();
    leftright::init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{ tm_hook_exit_thread(); }

static void *real_tm_get_thread_state() {
    return (void*)&g_tm_thread_state;
}



} // extern "C" — non-hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void *real_tm_get_env() { return (void*)&tm_jmpbuf; }

static void real_tm_set_jmpbuf(void *buf) { leftright::jmpbuf_ptr = (sigjmp_buf *)buf; }

// Forward declarations for static hook functions
static void real_tm_free(void *ptr);

static void real_tm_begin() {
    int32_t tc = tm_nested_call_counter;
    int32_t sc = g_tm_thread_state.nested_call_counter;
    int32_t c = (tc > 0) ? tc : sc;
    if (c <= 1) {
        g_in_tx = true;
        tm_clear_spec_allocs();
        tm_clear_deferred_frees();
        leftright::begin();
    }
}

static void real_tm_end() {
    int32_t tc = tm_nested_call_counter;
    int32_t sc = g_tm_thread_state.nested_call_counter;
    int32_t c = (tc > 0) ? tc : sc;
    if (c <= 1) {
        leftright::commit();
        tm_flush_deferred_frees();
        tm_flush_spec_allocs();
        g_in_tx = false;
    }
}

static void *real_tm_malloc(size_t size) {
    void *p = stm::tm_region_malloc(size);
    if (p) {
        std::memset(p, 0, size);
        tm_track_spec_alloc(p);
    }
    return p;
}

static void *real_tm_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = stm::tm_region_malloc(total);
    if (p) {
        std::memset(p, 0, total);
        tm_track_spec_alloc(p);
    }
    return p;
}

static void *real_tm_realloc(void *ptr, size_t size) {
    if (!ptr) return real_tm_malloc(size);
    void *p = stm::tm_region_malloc(size);
    if (p) {
        std::memcpy(p, ptr, size);
        if (stm::isTMAddress(ptr))
            stm::tm_region_free(ptr);
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
        stm::tm_region_free(ptr);
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
    .get_env    = real_tm_get_env,
    .set_jmpbuf = real_tm_set_jmpbuf,
    .get_thread_state = real_tm_get_thread_state,
};
