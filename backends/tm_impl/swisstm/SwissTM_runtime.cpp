#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <unordered_set>

#include "SwissTM.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_thread_state.hpp"
#include "tm_hooks.hpp"
#include "tm_backend_macros.hpp"

// Shared TLS variables (defined in tm_hooks.cpp, used by all backends)
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

extern const TMRealHooks g_swisstm_hooks;



extern "C" {

// Thread-local state
// NOTE: tm_jmpbuf must NOT be static — the plugin creates its own
// thread-local symbol with the same name, and the linker aliases them.
// siglongjmp to this buffer jumps back to the plugin's sigsetjmp.
__thread int8_t tm_is_init_ready = 0;
static thread_local TMThreadState g_tm_state{0, 0};

static void *real_tm_get_thread_state() {
    return (void*)&g_tm_state;
}

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
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
        fprintf(stderr, "FATAL: tm_region_init() failed — TM address space unavailable\n");
        std::abort();
    }
	swisstm::init();
	tm_register_real_hooks(&g_swisstm_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    stm::tm_region_destroy();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
	tm_hook_init_thread();
	swisstm::init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{ tm_hook_exit_thread(); swisstm::exit_thread(); }





} // extern "C" — non‑hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void *real_tm_get_env() { return (void*)&tm_jmpbuf; }

static void real_tm_set_jmpbuf(void *buf) { swisstm::set_jmpbuf((sigjmp_buf *)buf); }

static void real_tm_begin()
{
	swisstm::set_jmpbuf(&tm_jmpbuf);
	tm_clear_spec_allocs();
	tm_clear_deferred_frees();
	g_in_tx = true;
	swisstm::begin();
	assert(g_tm_state.nested_call_counter >= 0);
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

static void real_tm_end()
{
	swisstm::commit();
	g_in_tx = false;
	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
	assert(g_tm_state.nested_call_counter >= 0);
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
}

TM_DEFINE_READ_WRITE_HOOKS(swisstm)

static void *real_tm_malloc(size_t size) { return tm_track_alloc_result(stm::tm_region_malloc(size), size); }
static void *real_tm_calloc(size_t nmemb, size_t size) { void* p = stm::tm_region_malloc(nmemb * size); memset(p, 0, nmemb * size); return tm_track_alloc_result(p, nmemb * size); }
static void *real_tm_realloc(void* ptr, size_t size) { void* p = stm::tm_region_malloc(size); if (ptr) { memcpy(p, ptr, size); stm::tm_region_free(ptr); } return tm_track_alloc_result(p, size); }
static void  real_tm_free(void* ptr) {
	if (!ptr || !stm::isTMAddress(ptr)) return;
	TM_EVENT(FREE, ptr, 0);
	if (g_in_tx) {
		swisstm::tm_write_i1(reinterpret_cast<uint8_t*>(ptr), 0);
		tm_free_append_deferred(ptr);
	} else {
		stm::tm_region_free(ptr);
	}
}

// ═══════════════════════════════════════════════════════════════════
//  Plugin‑specific extern "C" functions (not hooks)
// ═══════════════════════════════════════════════════════════════════

extern "C" {

TM_DEFINE_PLUGIN_RW(swisstm)

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {}

} // extern "C"



// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

TM_REAL_HOOKS_TABLE_EXT(swisstm)
