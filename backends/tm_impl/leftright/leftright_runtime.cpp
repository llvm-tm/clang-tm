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
#include "../common/tm_backend_macros.hpp"
#include "../common/tm_thread_state.hpp"
#include "../common/tm_hooks.hpp"


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

TM_DEFINE_READ_WRITE_HOOKS_WITH_I8_CAST(leftright, static_cast<uint64_t>(v))

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

TM_REAL_HOOKS_TABLE_EXT(leftright);
