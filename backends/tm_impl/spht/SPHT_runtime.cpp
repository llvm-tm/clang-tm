/**
 * SPHT Runtime Wrapper for LLVM TM Plugin
 *
 * Uses Intel RTM + per-thread commit log with epoch-based group commit
 * for scalable persistent hardware transactions.
 */

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <mutex>
#include <unordered_set>
#include <unistd.h>
#include <new>

#include "tm_common.hpp"
#include "tm_thread_state.hpp"
#include "spht_globals.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_backend_macros.hpp"
#include "tm_hooks.hpp"

// SGL fallback for when RTM is broken / unavailable
static std::mutex g_spht_fallback_mutex;
thread_local bool g_spht_rtm_mode = false;

extern const TMRealHooks g_spht_hooks;

extern "C" {

extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

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
	tm_register_real_hooks(&g_spht_hooks);
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed — TM address space unavailable\n");
        std::abort();
    }
	spht::init();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
	spht::exit();
    stm::tm_region_destroy();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
	tm_hook_init_thread();
	spht::init_thread();
	spht::jmpbuf = &tm_jmpbuf;
	sigsetjmp(tm_jmpbuf, 0);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{
	tm_hook_exit_thread();
	spht::exit_thread();
}

static void *real_tm_get_thread_state()
{
	static thread_local TMThreadState ts;
	ts.nested_call_counter = tm_nested_call_counter;
	ts.longjmp_ret = tm_longjmp_ret;
	return &ts;
}

} // extern "C" — infrastructure

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void *real_tm_get_env() { return (void*)&tm_jmpbuf; }

static void real_tm_set_jmpbuf(void *buf) { spht::jmpbuf = (sigjmp_buf *)buf; }

static void
spht_push_malloc_entry(void *ptr, size_t size)
{
	if (g_in_tx && spht::current_tx && spht::current_tx->pcl) {
		stm::any_type_t w;
		w.u8 = (uint64_t)size;
		spht::current_tx->pcl->append(ptr, stm::ValueType::UINT64, w,
		                              spht::LOG_MALLOC);
	}
}

static void
spht_push_free_entry(void *ptr)
{
	if (g_in_tx && spht::current_tx && spht::current_tx->pcl) {
		stm::any_type_t w = {};
		spht::current_tx->pcl->append(ptr, stm::ValueType::UINT64, w,
		                              spht::LOG_FREE);
	}
}

static void real_tm_begin()
{
	tm_clear_spec_allocs();
	tm_clear_deferred_frees();
	g_in_tx = true;

	if (spht::begin()) {
		g_spht_rtm_mode = true;
	} else if (tm_longjmp_ret != 0) {
		g_spht_rtm_mode = false;
	} else {
		g_spht_rtm_mode = false;
		g_spht_fallback_mutex.lock();
	}

	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

static void real_tm_end()
{
	if (g_spht_rtm_mode) {
		spht::commit();
	} else {
		g_spht_fallback_mutex.unlock();
	}
	g_in_tx = false;
	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
}

TM_DEFINE_READ_WRITE_HOOKS_WITH_I8_CAST(spht, (uint64_t)v)

static void *real_tm_malloc(size_t size)
{
	void *p = stm::tm_region_malloc(size);
	tm_track_spec_alloc(p);
	spht_push_malloc_entry(p, size);
	return p;
}

static void *real_tm_calloc(size_t nmemb, size_t size)
{
	void *p = stm::tm_region_malloc(nmemb * size);
	memset(p, 0, nmemb * size);
	tm_track_spec_alloc(p);
	spht_push_malloc_entry(p, nmemb * size);
	return p;
}

static void *real_tm_realloc(void *ptr, size_t size)
{
	void *p = stm::tm_region_malloc(size);
	if (ptr) {
		memcpy(p, ptr, size);
		spht_push_free_entry(ptr);
		stm::tm_region_free(ptr);
	}
	tm_track_spec_alloc(p);
	spht_push_malloc_entry(p, size);
	return p;
}

static void real_tm_free(void *ptr)
{
	if (!ptr) return;
	if (!stm::isTMAddress(ptr)) return;
	spht_push_free_entry(ptr);
	if (g_in_tx) {
		spht::tm_write_i1(reinterpret_cast<uint8_t *>(ptr), 0);
		if (g_deferred_frees_set.count(ptr)) {
			TM_EVENT(DOUBLE_FREE, ptr, 0);
			fprintf(stderr, "FATAL: double-free detected in TM: ptr=%p\n", ptr);
			void *buf[64];
			int n = backtrace(buf, 64);
			backtrace_symbols_fd(buf, n, 2);
			fflush(stderr);
			_exit(1);
		}
		tm_untrack_spec_alloc(ptr);
		g_deferred_frees_set.insert(ptr);
		auto *node = static_cast<FreeNode *>(std::malloc(sizeof(FreeNode)));
		node->ptr = ptr;
		node->next = g_deferred_frees;
		g_deferred_frees = node;
	} else {
		stm::tm_region_free(ptr);
	}
}

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

TM_REAL_HOOKS_TABLE_EXT(spht)

// ═══════════════════════════════════════════════════════════════════
//  Plugin-specific extern "C" functions (not hook candidates)
// ═══════════════════════════════════════════════════════════════════

extern "C" {

TM_DEFINE_PLUGIN_RW(spht)

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) { (void)symbol_table; (void)symbol_count; }
void consume_ptr(volatile void *ptr) { (void)ptr; }

} // extern "C" — plugin-specific
