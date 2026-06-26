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
#include "../common/tm_backend_macros.hpp"
#include "tm_hooks.hpp"


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
    stm::tm_region_init();
    xtm::init();
    tm_register_real_hooks(&g_xtm_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
extern "C" void tm_exit()
#endif
{
    xtm::exit();
    if (auto ac = xtm::g_abort_counter.load(); ac > 0) {
        fprintf(stderr, "\n=== XTM total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
extern "C" void tm_init_thread()
#endif
{
    tm_hook_init_thread();
    xtm::init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
extern "C" void tm_exit_thread()
#endif
{ tm_hook_exit_thread(); }

static void real_tm_begin() {
    if (tm_nested_call_counter == 1) {
        g_in_tx = true;
        tm_clear_spec_allocs();
        tm_clear_deferred_frees();
        xtm::jmpbuf_ptr = (sigjmp_buf *)&tm_jmpbuf;
        xtm::begin();
    }
}

static void real_tm_end() {
    if (tm_nested_call_counter == 1) {
        xtm::commit();
        tm_flush_deferred_frees();
        tm_flush_spec_allocs();
        g_in_tx = false;
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
    if (g_in_tx) {
        tm_free_append_deferred(ptr);
    } else {
        tm_untrack_spec_alloc(ptr);
        if (stm::isTMAddress(ptr))
            stm::tm_region_free(ptr);
        else
            ::operator delete(ptr);
    }
}

TM_DEFINE_READ_WRITE_HOOKS(xtm)

static void *real_tm_get_thread_state() {
    return (void*)&tm_nested_call_counter;
}

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

TM_REAL_HOOKS_TABLE(xtm);
