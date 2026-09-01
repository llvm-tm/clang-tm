#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <unistd.h>
#include <mutex>
#include <new>
#include "NOrec_globals.hpp"
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

// Debug: catch absurd operator new sizes with backtrace
#ifndef NOREC_AS_WRAPPER
void* operator new(size_t size) {
    if (size > (1ULL << 42)) { // > 4TB
        fprintf(stderr, "\nFATAL: operator new(%zu) called with absurd size!\n", size);
        fflush(stderr);
        stm::tm_backtrace_print(2);
        _exit(1);
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t size) {
    if (size > (1ULL << 42)) {
        fprintf(stderr, "\nFATAL: operator new[](%zu) called with absurd size!\n", size);
        fflush(stderr);
        stm::tm_backtrace_print(2);
        _exit(1);
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif // !NOREC_AS_WRAPPER

thread_local bool g_in_tx = false;
thread_local FreeNode* g_deferred_frees = nullptr;
thread_local std::unordered_set<void*> g_deferred_frees_set;
thread_local SpecAlloc* g_spec_allocs = nullptr;

extern const TMRealHooks g_norec_hooks;

extern "C" {
// Thread-local state
static __thread int8_t tm_is_init_ready = 0;

// Retry loop support: returns true if retry via longjmp, false if new transaction
static inline bool check_retry_or_init()
{
	if (tm_longjmp_ret != 0) {
		// sigsetjmp was called and returned non-zero = retry from abort
		// norec::begin() has already been called by the retry path
		return true;
	}
	// First time or no plugin: set up setjmp if not done
	if (!tm_is_init_ready) {
		norec::jmpbuf = &tm_jmpbuf;
		tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
		tm_is_init_ready = 1;
	}
	return tm_longjmp_ret != 0;
}

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_tx_count{0};

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
    norec::init();
    tm_register_real_hooks(&g_norec_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    norec::exit();
    stm::tm_region_destroy();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
	tm_hook_init_thread();
	norec::init_thread();
	// Set NOrec's jmpbuf pointer to our thread-local buffer
	norec::jmpbuf = &tm_jmpbuf;
	// Set up the jump point for transaction aborts
	sigsetjmp(tm_jmpbuf, 0);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{ tm_hook_exit_thread(); norec::exit_thread(); }

static void *real_tm_get_thread_state() {
    return (void*)&tm_nested_call_counter;
}

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }

void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_serialize_unlock_all()
{
    if (g_serialize_mutex.try_lock()) {
        g_serialize_mutex.unlock();
        return 1;
    }
    return 0;
}

int tm_setjmp()
{
	// This should NOT be called - the plugin injects setjmp
	// Just return 0 to satisfy the linker
	return 0;
}



void tm_set_env(sigjmp_buf *env)
{
	if (env) {
		memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
		norec::jmpbuf = &tm_jmpbuf;
		sigsetjmp(tm_jmpbuf, 0);
	}
}

} // extern "C" — non‑hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void real_tm_begin()
{
	if (tm_longjmp_ret == 0) {
		tm_clear_spec_allocs();
		tm_clear_deferred_frees();
		g_in_tx = true;
		norec::begin();
	}
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

static void real_tm_end()
{
	norec::commit();
	g_in_tx = false;
	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
	g_tm_tx_count.fetch_add(1, std::memory_order_relaxed);
}

TM_DEFINE_READ_WRITE_HOOKS_WITH_I8_CAST(norec, static_cast<uint64_t>(v))

// TM allocator stubs.
static void* real_tm_malloc(size_t size) {
    void* p = stm::tm_region_malloc(size);
    std::memset(p, 0, size);
    return tm_track_alloc_result(p, size);
}
static void* real_tm_calloc(size_t nmemb, size_t size) {
    void* p = stm::tm_region_malloc(nmemb * size);
    std::memset(p, 0, nmemb * size);
    return tm_track_alloc_result(p, nmemb * size);
}
static void* real_tm_realloc(void* ptr, size_t size) {
    if (!ptr) return real_tm_malloc(size);
    void* p = stm::tm_region_malloc(size);
    if (p) {
        std::memcpy(p, ptr, size);
        stm::tm_region_free(ptr);
    }
    return tm_track_alloc_result(p, size);
}
static void  real_tm_free(void* ptr) {
    if (!ptr || !stm::isTMAddress(ptr)) return;
    TM_EVENT(FREE, ptr, 0);
    if (g_in_tx) {
        norec::tm_write_i1(reinterpret_cast<uint8_t*>(ptr), 0);
        tm_free_append_deferred(ptr);
    } else {
        stm::tm_region_free(ptr);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Plugin‑specific extern "C" functions (not hooks)
// ═══════════════════════════════════════════════════════════════════

extern "C" {

TM_DEFINE_PLUGIN_RW(norec)

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

TM_REAL_HOOKS_TABLE(norec)
