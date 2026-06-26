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
#include "tm_backend_macros.hpp"
#include "tm_hooks.hpp"
extern const TMRealHooks g_romulus_hooks;


namespace romulus {

std::atomic<uint64_t> g_global_clock{1};
std::atomic<uint64_t> thr_counter{1};
__thread Transaction *current_tx = nullptr;
std::atomic<uint64_t> g_tm_abort_count{0};
std::atomic<uint64_t> *g_version_table = nullptr;
std::atomic<uint64_t> g_commit_lock{0};
__thread sigjmp_buf *jmpbuf_ptr;

} // namespace romulus

__thread sigjmp_buf *jmpbuf;

extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}
__thread int tm_init_thread_call_count = 0;

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
    romulus::init();
    // Mark as multi-thread so real hooks (region allocator) are installed
    // immediately, even before any worker thread calls tm_init_thread().
    // Without this, s_thread_count=1 causes stubs (std::malloc) to be used,
    // and TM-object pointers allocated during Bank construction land on the
    // regular heap — making isTMAddress() return false during read_word.
    tm_set_num_threads(2);
    tm_register_real_hooks(&g_romulus_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    romulus::exit();
    if (auto ac = romulus::g_tm_abort_count.load(); ac > 0) {
        fprintf(stderr, "\n=== Romulus total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{ tm_hook_init_thread(); romulus::init_thread(); }

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{ tm_hook_exit_thread(); }

} // extern "C" — non-hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void real_tm_begin() {
    if (tm_nested_call_counter == 1) {
        romulus::jmpbuf_ptr = (sigjmp_buf *)&tm_jmpbuf;
        romulus::begin();
    }
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

static void real_tm_free(void *ptr) {
    if (!ptr) return;
    tm_untrack_spec_alloc(ptr);
    if (g_in_tx)
        tm_free_append_deferred(ptr);
    else
        stm::tm_region_free(ptr);
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

TM_DEFINE_READ_WRITE_HOOKS_WITH_I8_CAST(romulus, (uint64_t)v)

static void *real_tm_get_thread_state() {
    return (void*)&tm_nested_call_counter;
}

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

TM_REAL_HOOKS_TABLE(romulus);
