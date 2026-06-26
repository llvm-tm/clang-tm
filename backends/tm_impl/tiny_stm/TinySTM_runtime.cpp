/**
 * TinySTM Runtime Wrapper for LLVM TM Plugin
 * Uses TinySTM for transactional memory
 */

extern "C" {
void tm_serialize_lock();
void tm_serialize_unlock();
int tm_serialize_unlock_all();
}

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <mutex>
#include <new>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

#include "tinystm_globals.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_backend_macros.hpp"
#include "tm_thread_state.hpp"
#include "tm_hooks.hpp"

// Shared TLS variables (defined in tm_hooks.cpp, used by all backends)
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

extern const TMRealHooks g_tinystm_hooks;
thread_local bool g_tm_expli_mode = false;

// EBR retired list
thread_local FreeNode *g_retired_frees = nullptr;

// Per-thread index for EBR version tracking.
thread_local size_t g_tl_tid = 0;

// Global array: g_thread_tx_version[tid] = start_version of current TX, or 0 if no TX.
// (tid is the thread's TinySTM tx->id, which is 1-based.)
std::atomic<uint64_t> g_thread_tx_version[tinystm::MAX_THREADS];

// Global set: given pointer currently tracked by exactly one thread's retired list.
// tm_move_deferred_to_retired inserts here; tm_flush_retired_frees erases after free.
// Ensures the same shared buffer (e.g. std::vector old buffer) is freed only once
// when both threads independently defer it.
std::mutex g_retired_global_mutex;
std::unordered_set<void *> g_retired_global_set;

static pthread_once_t g_tm_state_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_tm_state_key;

static void make_tm_state_key() { pthread_key_create(&g_tm_state_key, NULL); }

__thread int tm_init_thread_call_count = 0;

// Mutex removed — TinySTM's own lock acquisition handles concurrency,
// and a std::mutex around commit() leaks across siglongjmp from abort_tx()

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

thread_local uint64_t tm_begin_count{0};
thread_local uint64_t tm_end_count{0};
thread_local uint64_t tm_tx_count{0};

std::atomic<uint64_t> g_tm_max_read_set{0};
std::atomic<uint64_t> g_tm_max_write_set{0};
std::atomic<uint64_t> g_tm_min_read_set{UINT64_MAX};
std::atomic<uint64_t> g_tm_min_write_set{UINT64_MAX};
std::atomic<uint64_t> g_tm_total_commit_reads{0};
std::atomic<uint64_t> g_tm_total_commit_writes{0};
std::atomic<uint64_t> g_tm_commit_count{0};
thread_local uint64_t g_tm_tx_read_set{0};
thread_local uint64_t g_tm_tx_write_set{0};
static std::atomic<uint64_t> g_tm_begin_count{0};
static std::atomic<uint64_t> g_tm_end_count{0};
static std::atomic<uint64_t> g_tm_tx_count{0};
extern "C" {
#ifdef LLVM_TM_PLUGIN

static void do_tm_init()
{
	tinystm::init();
	tinystm::reset_locks();
	tm_register_real_hooks(&g_tinystm_hooks);
}

static void do_tm_exit()
{
	tinystm::exit();
	auto cc = g_tm_commit_count.load();
	auto tr = g_tm_total_commit_reads.load();
	auto tw = g_tm_total_commit_writes.load();
	auto mxr = g_tm_max_read_set.load();
	auto mxw = g_tm_max_write_set.load();
	auto mnr = g_tm_min_read_set.load();
	auto mnw = g_tm_min_write_set.load();
	auto ac = tinystm::g_tm_abort_count.load();

	fprintf(stdout, "TM_STATS: commits=%llu", (unsigned long long)cc);
	if (cc > 0) {
		fprintf(stdout,
		        " avg_reads=%.1f min_reads=%llu max_reads=%llu"
		        " avg_writes=%.1f min_writes=%llu max_writes=%llu",
		        (double)tr / cc,
		        (unsigned long long)mnr,
		        (unsigned long long)mxr,
		        (double)tw / cc,
		        (unsigned long long)mnw,
		        (unsigned long long)mxw);
	}
	fprintf(stdout, " aborts=%llu\n", (unsigned long long)ac);
	fflush(stdout);

	if (ac > 0) {
		fprintf(stderr,
		        "\n=== TinySTM total aborts = %llu ===\n",
		        (unsigned long long)ac);
	}
}

static void do_tm_init_thread()
{
	tm_hook_init_thread();
	tm_init_thread_call_count++;
#ifndef NDEBUG
	(void)0;
#endif
	tinystm::init_thread();
#if defined(DESIGN_WBCTL)
	g_tl_tid = tinystm::current_tx_wbctl->id;
#elif defined(DESIGN_WBETL)
	g_tl_tid = tinystm::current_tx_wbetl->id;
#elif defined(DESIGN_WT)
	g_tl_tid = tinystm::current_tx_wt->id;
#endif
}

static void do_tm_exit_thread()
{
	tm_hook_exit_thread();
}

void (*tm_init)()        = do_tm_init;
void (*tm_exit)()        = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;

#else

void tm_init()
{
	tinystm::init();
	tinystm::reset_locks();
	tm_register_real_hooks(&g_tinystm_hooks);
}

void tm_exit()
{
	tinystm::exit();
	auto cc = g_tm_commit_count.load();
	auto tr = g_tm_total_commit_reads.load();
	auto tw = g_tm_total_commit_writes.load();
	auto mxr = g_tm_max_read_set.load();
	auto mxw = g_tm_max_write_set.load();
	auto mnr = g_tm_min_read_set.load();
	auto mnw = g_tm_min_write_set.load();
	auto ac = tinystm::g_tm_abort_count.load();

	fprintf(stdout, "TM_STATS: commits=%llu", (unsigned long long)cc);
	if (cc > 0) {
		fprintf(stdout,
		        " avg_reads=%.1f min_reads=%llu max_reads=%llu"
		        " avg_writes=%.1f min_writes=%llu max_writes=%llu",
		        (double)tr / cc,
		        (unsigned long long)mnr,
		        (unsigned long long)mxr,
		        (double)tw / cc,
		        (unsigned long long)mnw,
		        (unsigned long long)mxw);
	}
	fprintf(stdout, " aborts=%llu\n", (unsigned long long)ac);
	fflush(stdout);

	if (ac > 0) {
		fprintf(stderr,
		        "\n=== TinySTM total aborts = %llu ===\n",
		        (unsigned long long)ac);
	}
}

void tm_init_thread()
{
	tm_hook_init_thread();
	tm_init_thread_call_count++;
#ifndef NDEBUG
	(void)0;
#endif
	tinystm::init_thread();
#if defined(DESIGN_WBCTL)
	g_tl_tid = tinystm::current_tx_wbctl->id;
#elif defined(DESIGN_WBETL)
	g_tl_tid = tinystm::current_tx_wbetl->id;
#elif defined(DESIGN_WT)
	g_tl_tid = tinystm::current_tx_wt->id;
#endif
}

void tm_exit_thread()
{
	tm_hook_exit_thread();
	// no-op — Transaction object persists for the thread's lifetime
}

#endif

// Serialization lock for functions that couldn't be cloned.
// Uses recursive_mutex so the same thread can re-acquire after a longjmp retry.
// The lock count is tracked thread-locally so abort_tx can clean it up before
// siglongjmp (preventing permanent recursive-mutex leaks across aborts).
static std::recursive_mutex g_serialize_mutex;
static thread_local int g_serialize_lock_count = 0;

void tm_serialize_lock()
{
	g_serialize_mutex.lock();
	g_serialize_lock_count++;
}

void tm_serialize_unlock()
{
	g_serialize_lock_count--;
	g_serialize_mutex.unlock();
}

// Called from abort_tx before siglongjmp to release the serialize lock.
// Returns the number of unlocks performed (for assertion/debug).
int tm_serialize_unlock_all()
{
	int n = g_serialize_lock_count;
	for (int i = 0; i < n; i++)
		g_serialize_mutex.unlock();
	g_serialize_lock_count = 0;
	return n;
}

// Separate thread-local for plugin-side TM state so that
// tm_nested_call_counter (expli API) and ts->nested_call_counter
// (plugin entry sequence) are distinct — the mode detection in
// tm_begin() compares both to decide explicit vs plugin mode.
static __thread TMThreadState g_tm_thread_state = {0, 0};

} // extern "C" — non‑hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void *real_tm_get_env() {
    return (void*)&tm_jmpbuf;
}

static void real_tm_set_jmpbuf(void *buf) {
    tinystm::jmpbuf = (sigjmp_buf *)buf;
}

static void *real_tm_get_thread_state() {
    return &g_tm_thread_state;
}

static void real_tm_begin()
{
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
	tm_begin_count++;
	auto *ts = real_tm_get_thread_state();
	int32_t tc = tm_nested_call_counter;
	int32_t sc = ((TMThreadState*)ts)->nested_call_counter;
	if (tc > 0 && sc == 0)
		g_tm_expli_mode = true;
	else if (sc > 0 && tc == 0)
		g_tm_expli_mode = false;
	int32_t c = (tc > 0) ? tc : sc;
	if (c == 1) {
		g_in_tx = true;
		tinystm::jmpbuf = (sigjmp_buf *)&tm_jmpbuf;
		tm_clear_spec_allocs();
		tm_clear_deferred_frees();
		tinystm::begin();
		g_thread_tx_version[g_tl_tid] = 0;
		uint64_t safe_version = UINT64_MAX;
		for (size_t i = 1; i < tinystm::MAX_THREADS; i++) {
			uint64_t v = g_thread_tx_version[i].load(std::memory_order_acquire);
			if (v != 0 && v < safe_version)
				safe_version = v;
		}
		tm_flush_retired_frees(safe_version);
#if defined(DESIGN_WBCTL)
		auto *tx = tinystm::current_tx_wbctl;
#elif defined(DESIGN_WBETL)
		auto *tx = tinystm::current_tx_wbetl;
#elif defined(DESIGN_WT)
		auto *tx = tinystm::current_tx_wt;
#endif
		g_thread_tx_version[g_tl_tid].store(tx->start_version, std::memory_order_release);
	}
	assert(c >= 0);
}

static void real_tm_end()
{
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
	tm_end_count++;
	auto *ts = real_tm_get_thread_state();
	int32_t tc = tm_nested_call_counter;
	int32_t sc = ((TMThreadState*)ts)->nested_call_counter;
	int32_t c = (tc > 0) ? tc : sc;
	if (c == 1) {
		g_in_tx = false;
#if defined(DESIGN_WBCTL)
		auto *tx = tinystm::current_tx_wbctl;
#elif defined(DESIGN_WBETL)
		auto *tx = tinystm::current_tx_wbetl;
#elif defined(DESIGN_WT)
		auto *tx = tinystm::current_tx_wt;
#endif
		if (tx) {
			uint64_t rs = tx->read_set.size();
			uint64_t ws = tx->write_set.size();
			if (rs > g_tm_max_read_set.load())
				g_tm_max_read_set.store(rs);
			if (ws > g_tm_max_write_set.load())
				g_tm_max_write_set.store(ws);
			if (rs < g_tm_min_read_set.load())
				g_tm_min_read_set.store(rs);
			if (ws < g_tm_min_write_set.load())
				g_tm_min_write_set.store(ws);
			g_tm_total_commit_reads.fetch_add(rs, std::memory_order_relaxed);
			g_tm_total_commit_writes.fetch_add(ws, std::memory_order_relaxed);
			g_tm_commit_count.fetch_add(1, std::memory_order_relaxed);
		}
		tinystm::commit();
		tm_move_deferred_to_retired(tx->commit_version);
		tm_flush_spec_allocs();
		g_thread_tx_version[g_tl_tid].store(0, std::memory_order_release);
	}
	assert(c >= 0);
	tm_tx_count++;
}

TM_DEFINE_READ_WRITE_HOOKS(tinystm)

static void *real_tm_malloc(size_t size)
{
	void *p = stm::tm_region_malloc(size);
	memset(p, 0, size);
	tm_track_spec_alloc(p);
	return p;
}
static void *real_tm_calloc(size_t nmemb, size_t size)
{
	void *p = stm::tm_region_malloc(nmemb * size);
	memset(p, 0, nmemb * size);
	tm_track_spec_alloc(p);
	return p;
}
static void *real_tm_realloc(void *ptr, size_t size)
{
	void *p = stm::tm_region_malloc(size);
	memset(p, 0, size);
	if (ptr) {
		stm::tm_region_free(ptr);
	}
	tm_track_spec_alloc(p);
	return p;
}
static void real_tm_free(void *ptr)
{
	if (!ptr) return;
	if (g_in_tx) {
		if (g_deferred_frees_set.count(ptr)) {
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
		if (stm::isTMAddress(ptr))
			stm::tm_region_free(ptr);
		else
			::operator delete(ptr);
	}
}

// ═══════════════════════════════════════════════════════════════════
//  Plugin‑specific extern "C" functions (not hooks)
// ═══════════════════════════════════════════════════════════════════

extern "C" {

TM_DEFINE_PLUGIN_RW(tinystm)

void tm_load_symbols(void *symbol_table, uint32_t symbol_count)
{
	(void)symbol_table;
	(void)symbol_count;
}
void consume_ptr(volatile void *ptr) { (void)ptr; }



} // extern "C"

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

TM_REAL_HOOKS_TABLE_EXT(tinystm)

// ═══════════════════════════════════════════════════════════════════
//  operator delete overrides — handle TM-region pointers at exit
// ═══════════════════════════════════════════════════════════════════
//
// Global object destructors (e.g. ~std::vector<T>) call ::operator delete
// from libstdc++ without going through the plugin's tm_free replacement.
// If the buffer is in the TM region (mmap), the default operator delete
// calls free() on a non-heap address → "free(): invalid pointer".
//
// These overrides redirect TM-region pointers to tm_region_free and
// pass non-TM pointers to the standard deallocation path.
//
// These are NOT inline so the linker picks them as strong symbols.

static void tm_delete_impl(void *ptr) noexcept
{
	if (!ptr) return;
	if (stm::isTMAddress(ptr))
		stm::tm_region_free(ptr);
	else
		std::free(ptr);
}

void operator delete(void *ptr) noexcept { tm_delete_impl(ptr); }
void operator delete(void *ptr, size_t) noexcept { tm_delete_impl(ptr); }
void operator delete(void *ptr, std::align_val_t) noexcept { tm_delete_impl(ptr); }
void operator delete(void *ptr, size_t, std::align_val_t) noexcept { tm_delete_impl(ptr); }
void operator delete[](void *ptr) noexcept { tm_delete_impl(ptr); }
void operator delete[](void *ptr, size_t) noexcept { tm_delete_impl(ptr); }
void operator delete[](void *ptr, std::align_val_t) noexcept { tm_delete_impl(ptr); }
void operator delete[](void *ptr, size_t, std::align_val_t) noexcept { tm_delete_impl(ptr); }
