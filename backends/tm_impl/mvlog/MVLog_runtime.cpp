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
#include "MVLog_globals.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_thread_state.hpp"
#include "tm_hooks.hpp"

// Shared TLS variables (defined in tm_hooks.cpp, used by all backends)
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// Debug: catch absurd operator new sizes with backtrace
#ifndef MVLOG_AS_WRAPPER
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
#endif // !MVLOG_AS_WRAPPER

thread_local bool g_in_tx = false;
thread_local FreeNode* g_deferred_frees = nullptr;
thread_local std::unordered_set<void*> g_deferred_frees_set;
thread_local SpecAlloc* g_spec_allocs = nullptr;

extern const TMRealHooks g_mvlog_hooks;

extern "C" {
// Thread-local state
static __thread int8_t tm_is_init_ready = 0;

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
    mvlog::init();
    tm_register_real_hooks(&g_mvlog_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    mvlog::exit();
    stm::tm_region_destroy();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
	tm_hook_init_thread();
	mvlog::init_thread();
	// Point MVLog's jmpbuf at our thread-local buffer so abort_tx() longjmps
	// to the retry loop's sigsetjmp.
	mvlog::jmpbuf = &tm_jmpbuf;
	sigsetjmp(tm_jmpbuf, 0);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{ tm_hook_exit_thread(); mvlog::exit_thread(); }

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
		mvlog::jmpbuf = &tm_jmpbuf;
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
	}
	g_in_tx = true;
	mvlog::begin();
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

static void real_tm_end()
{
	mvlog::commit();
	g_in_tx = false;
	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
	g_tm_tx_count.fetch_add(1, std::memory_order_relaxed);
}

static uint8_t real_tm_read_i1(uint8_t *addr) { return mvlog::tm_read_i1(addr); }

static uint16_t real_tm_read_i2(uint16_t *addr)
{
	return mvlog::tm_read_i2(addr);
}

static uint32_t real_tm_read_i4(uint32_t *addr)
{
	return mvlog::tm_read_i4(addr);
}

static uint64_t real_tm_read_i8(uint64_t *addr)
{
	return mvlog::tm_read_i8(addr);
}

static float real_tm_read_f4(float *addr) { return mvlog::tm_read_f4(addr); }

static double real_tm_read_f8(double *addr) { return mvlog::tm_read_f8(addr); }

static void *real_tm_read_ptr(void **addr) {
    return mvlog::tm_read_ptr(addr);
}

static void real_tm_write_i1(uint8_t *addr, uint8_t val)
{
	mvlog::tm_write_i1(addr, val);
}

static void real_tm_write_i2(uint16_t *addr, uint16_t val)
{
	mvlog::tm_write_i2(addr, val);
}

static void real_tm_write_i4(uint32_t *addr, uint32_t val)
{
	mvlog::tm_write_i4(addr, val);
}

static void real_tm_write_i8(uint64_t *addr, int64_t val)
{
	mvlog::tm_write_i8(addr, static_cast<uint64_t>(val));
}

static void real_tm_write_f4(float *addr, float val)
{
	mvlog::tm_write_f4(addr, val);
}

static void real_tm_write_f8(double *addr, double val)
{
	mvlog::tm_write_f8(addr, val);
}

static void real_tm_write_ptr(void **addr, void *val)
{
	mvlog::tm_write_ptr(addr, val);
}

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
        mvlog::tm_write_i1(reinterpret_cast<uint8_t*>(ptr), 0);
        tm_free_append_deferred(ptr);
    } else {
        stm::tm_region_free(ptr);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Plugin‑specific extern "C" functions (not hooks)
// ═══════════════════════════════════════════════════════════════════

extern "C" {

void tm_read_i16(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    out_words[0] = mvlog::tm_read_i8(static_cast<uint64_t *>(addr) + 0);
    out_words[1] = mvlog::tm_read_i8(static_cast<uint64_t *>(addr) + 1);
}

void tm_read_i32(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    for (int i = 0; i < 4; i++)
        out_words[i] = mvlog::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}

void tm_read_i64(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    for (int i = 0; i < 8; i++)
        out_words[i] = mvlog::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}

void tm_write_i16(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 2; i++)
        mvlog::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_i32(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 4; i++)
        mvlog::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_i64(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 8; i++)
        mvlog::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void *tm_read_z(uint8_t *addr, uint64_t len)
{
	assert(len < TM_BUFFER_SIZE);
	for (uint64_t i = 0; i < len / 8; i++) {
		tm_buffer[i] = mvlog::tm_read_i8(((uint64_t *)addr) + i);
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) { // rem > 0
		tm_buffer[i] = mvlog::tm_read_i1(addr + (len - rem - 1) + i);
	}
	return tm_buffer;
}

void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len)
{
	for (uint64_t i = 0; i < len / 8; i++) {
		mvlog::tm_write_i8(((uint64_t *)dst) + i, *(((uint64_t *)src) + i));
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) { // rem > 0
		mvlog::tm_write_i1(dst + (len - rem - 1) + i, *(src + (len - rem - 1) + i));
	}
}

void tm_memset(uint8_t *addr, uint8_t val, uint64_t len)
{
	for (uint64_t i = 0; i < len; i++) {
		mvlog::tm_write_i1(&addr[i], val);
	}
}

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

static void real_tm_set_jmpbuf(void *buf) {
    mvlog::jmpbuf = static_cast<sigjmp_buf *>(buf);
}

static void *real_tm_get_env() {
    return (void*)&tm_jmpbuf;
}

const TMRealHooks g_mvlog_hooks = {
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
    .get_env   = real_tm_get_env,
    .set_jmpbuf = real_tm_set_jmpbuf,
    .get_thread_state = real_tm_get_thread_state,
};
