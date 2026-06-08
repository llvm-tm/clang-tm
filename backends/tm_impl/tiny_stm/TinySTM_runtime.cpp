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
#include <new>
#include <execinfo.h>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

#include "tinystm_globals.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_thread_state.hpp"
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;
thread_local bool g_tm_expli_mode = false;
thread_local bool g_in_tx = false;
thread_local FreeNode* g_deferred_frees = nullptr;
thread_local std::unordered_set<void*> g_deferred_frees_set;
thread_local SpecAlloc* g_spec_allocs = nullptr;

// EBR retired list
thread_local FreeNode* g_retired_frees = nullptr;

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

static void make_tm_state_key() {
    pthread_key_create(&g_tm_state_key, NULL);
}

__thread sigjmp_buf tm_jmpbuf;
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

void tm_init() {
	tinystm::init();
	tinystm::reset_locks();
}

void tm_exit() {
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
		fprintf(stdout, " avg_reads=%.1f min_reads=%llu max_reads=%llu"
			" avg_writes=%.1f min_writes=%llu max_writes=%llu",
			(double)tr / cc, (unsigned long long)mnr, (unsigned long long)mxr,
			(double)tw / cc, (unsigned long long)mnw, (unsigned long long)mxw);
	}
	fprintf(stdout, " aborts=%llu\n", (unsigned long long)ac);
	fflush(stdout);

#ifndef NDEBUG
	fprintf(stderr, "\n=== TinySTM max read-set = %llu, max write-set = %llu ===\n",
		(unsigned long long)mxr, (unsigned long long)mxw);
#endif
	if (ac > 0) {
		fprintf(stderr, "\n=== TinySTM total aborts = %llu ===\n",
			(unsigned long long)ac);
	}
}

void tm_init_thread()
{
	tm_init_thread_call_count++;
#ifndef NDEBUG
	(void)0;
#endif
	tinystm::init_thread();
	// Capture the thread's TinySTM ID for EBR.
	// tinystm::init_thread() sets the thread ID (tx->id) via thr_counter.
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
	// no-op — Transaction object persists for the thread's lifetime
}

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

void tm_set_jmpbuf(void *buf) { tinystm::jmpbuf = (sigjmp_buf *)buf; }
sigjmp_buf *tm_get_env() { return &tm_jmpbuf; }

int tm_setjmp() { return 0; }

// Separate thread-local for plugin-side TM state so that
// tm_nested_call_counter (expli API) and ts->nested_call_counter
// (plugin entry sequence) are distinct — the mode detection in
// tm_begin() compares both to decide explicit vs plugin mode.
static __thread TMThreadState g_tm_thread_state = {0, 0};

TMThreadState *tm_get_thread_state() {
    return &g_tm_thread_state;
}

static __thread int tm_count = 0;

void tm_begin()
{
	int mc = ++tm_count;
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
	tm_begin_count++;
	auto *ts = tm_get_thread_state();
	int32_t tc = tm_nested_call_counter;
	int32_t sc = ts->nested_call_counter;
	if (tc > 0 && sc == 0)
		g_tm_expli_mode = true;
	else if (sc > 0 && tc == 0)
		g_tm_expli_mode = false;
	int32_t c = (tc > 0) ? tc : sc;
	fprintf(stderr, "[tm_begin #%d] sc=%d tc=%d c=%d tx_active=%d\n",
		mc, sc, tc, c,
		tinystm::current_tx_wbctl ? tinystm::current_tx_wbctl->active : -1);
	if (c == 1) { g_in_tx = true;
		tinystm::jmpbuf = (sigjmp_buf *)&tm_jmpbuf;
		tm_clear_spec_allocs();
		tm_clear_deferred_frees();
		tinystm::begin();
		// EBR: register our start version and flush safe retired entries.
		// On retry (siglongjmp), the stale version from the aborted TX
		// is still in the array; we clear it first to avoid inflating
		// the safe_version minimum.
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
		g_thread_tx_version[g_tl_tid].store(tx->start_version,
		                                    std::memory_order_release);
	}
	assert(c >= 0);
}

static __thread int tm_end_call_count = 0;

void tm_end()
{
	int my_call = ++tm_end_call_count;
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
	tm_end_count++;
	auto *ts = tm_get_thread_state();
	int32_t tc = tm_nested_call_counter;
	int32_t sc = ts->nested_call_counter;
	int32_t c = (tc > 0) ? tc : sc;
	if (c == 1) { 
		fprintf(stderr, "[tm_end #%d] sc=%d tc=%d c=%d calling commit (tx->active=%d)\n",
			my_call, sc, tc, c,
			tinystm::current_tx_wbctl ? tinystm::current_tx_wbctl->active : -1);
		g_in_tx = false;
		// Record max read-set and write-set sizes for this TX
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
			if (rs > g_tm_max_read_set.load()) g_tm_max_read_set.store(rs);
			if (ws > g_tm_max_write_set.load()) g_tm_max_write_set.store(ws);
			if (rs < g_tm_min_read_set.load()) g_tm_min_read_set.store(rs);
			if (ws < g_tm_min_write_set.load()) g_tm_min_write_set.store(ws);
			g_tm_total_commit_reads.fetch_add(rs, std::memory_order_relaxed);
			g_tm_total_commit_writes.fetch_add(ws, std::memory_order_relaxed);
			g_tm_commit_count.fetch_add(1, std::memory_order_relaxed);
		}
		tinystm::commit();
		tm_move_deferred_to_retired(tx->commit_version);
		tm_flush_spec_allocs();
		// Unregister from EBR version tracking
		g_thread_tx_version[g_tl_tid].store(0, std::memory_order_release);
	}
	assert(c >= 0);
	tm_tx_count++;
}

uint8_t tm_read_i1(uint8_t *addr) { return tinystm::tm_read_i1(addr); }
uint16_t tm_read_i2(uint16_t *addr) { return tinystm::tm_read_i2(addr); }
uint32_t tm_read_i4(uint32_t *addr) { return tinystm::tm_read_i4(addr); }
uint64_t tm_read_i8(uint64_t *addr) { return tinystm::tm_read_i8(addr); }
void tm_read_i16(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    out_words[0] = tinystm::tm_read_i8(static_cast<uint64_t *>(addr) + 0);
    out_words[1] = tinystm::tm_read_i8(static_cast<uint64_t *>(addr) + 1);
}
void tm_read_i32(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    for (int i = 0; i < 4; i++)
        out_words[i] = tinystm::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}
void tm_read_i64(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    for (int i = 0; i < 8; i++)
        out_words[i] = tinystm::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}
float tm_read_f4(float *addr) { return tinystm::tm_read_f4(addr); }
double tm_read_f8(double *addr) { return tinystm::tm_read_f8(addr); }
void *tm_read_ptr(void **addr) {
	return tinystm::tm_read_ptr(addr);
}

void tm_write_i1(uint8_t *addr, uint8_t val) { tinystm::tm_write_i1(addr, val); }
void tm_write_i2(uint16_t *addr, int16_t val) { tinystm::tm_write_i2(addr, val); }
void tm_write_i4(uint32_t *addr, int32_t val) { tinystm::tm_write_i4(addr, val); }
void tm_write_i8(uint64_t *addr, int64_t val) { tinystm::tm_write_i8(addr, val); }
void tm_write_i16(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 2; i++)
        tinystm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}
void tm_write_i32(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 4; i++)
        tinystm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}
void tm_write_i64(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 8; i++)
        tinystm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}
void tm_write_f4(float *addr, float val) { tinystm::tm_write_f4(addr, val); }
void tm_write_f8(double *addr, double val) { tinystm::tm_write_f8(addr, val); }
void tm_write_ptr(void **addr, void *val) {
	tinystm::tm_write_ptr(addr, val);
}

void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len) {
	for (uint64_t i = 0; i < len / 8; i++) {
		tinystm::tm_write_i8(((uint64_t *)dst) + i, *(((uint64_t *)src) + i));
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) {
		tinystm::tm_write_i1(dst + (len - rem - 1) + i, src[(len - rem - 1) + i]);
	}
}

void tm_memset(uint8_t *addr, uint8_t val, uint64_t len) {
	for (uint64_t i = 0; i < len; i++) {
		tinystm::tm_write_i1(&addr[i], val);
	}
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) { (void)symbol_table; (void)symbol_count; }
void consume_ptr(volatile void *ptr) { (void)ptr; }
void* tm_malloc(size_t size) { void* p = stm::tm_region_malloc(size); memset(p, 0, size); tm_track_spec_alloc(p); return p; }
void* tm_calloc(size_t nmemb, size_t size) { void* p = stm::tm_region_malloc(nmemb * size); memset(p, 0, nmemb * size); tm_track_spec_alloc(p); return p; }
void* tm_realloc(void* ptr, size_t size) {
	void* p = stm::tm_region_malloc(size);
	memset(p, 0, size);
	if (ptr) {
		stm::tm_region_free(ptr);
	}
	tm_track_spec_alloc(p);
	return p;
}
void  tm_free(void* ptr) {
	if (g_in_tx) {
		if (g_deferred_frees_set.count(ptr)) {
			fprintf(stderr, "FATAL: double-free detected in TM: ptr=%p\n", ptr);
			void* buf[64];
			int n = backtrace(buf, 64);
			backtrace_symbols_fd(buf, n, 2);
			fflush(stderr);
			_exit(1);
		}
		tm_untrack_spec_alloc(ptr);
		g_deferred_frees_set.insert(ptr);
		auto* node = static_cast<FreeNode*>(std::malloc(sizeof(FreeNode)));
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

} // extern "C"
