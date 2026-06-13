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

// Shared TLS variables (defined in tm_hooks.cpp, used by all backends)
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

extern const TMRealHooks g_swisstm_hooks;

thread_local bool g_in_tx = false;
thread_local FreeNode* g_deferred_frees = nullptr;
thread_local std::unordered_set<void*> g_deferred_frees_set;
thread_local SpecAlloc* g_spec_allocs = nullptr;

extern "C" {

// Thread-local state
// NOTE: tm_jmpbuf must NOT be static — the plugin creates its own
// thread-local symbol with the same name, and the linker aliases them.
// siglongjmp to this buffer jumps back to the plugin's sigsetjmp.
__thread int8_t tm_is_init_ready = 0;
static thread_local TMThreadState g_tm_state{0, 0};

TMThreadState *tm_get_thread_state() {
    return &g_tm_state;
}

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
void tm_init()
{
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed — TM address space unavailable\n");
        std::abort();
    }
	swisstm::init();
	tm_register_real_hooks(&g_swisstm_hooks);
}

void tm_exit()
{
    stm::tm_region_destroy();
}

void tm_init_thread()
{
	tm_hook_init_thread();
	swisstm::init_thread();
}

void tm_exit_thread() { tm_hook_exit_thread(); swisstm::exit_thread(); }

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }

void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp() { return 0; }

void tm_set_jmpbuf(void *buf) { swisstm::set_jmpbuf((sigjmp_buf *)buf); }

sigjmp_buf *tm_get_env() { return &tm_jmpbuf; }

void tm_set_env(sigjmp_buf *env)
{
	if (env) {
		memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
		tm_is_init_ready = 1;
	}
}

} // extern "C" — non‑hook functions above, hooks below

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

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

static uint8_t real_tm_read_i1(uint8_t *addr)
{
	return swisstm::tm_read_i1(addr);
}

static uint16_t real_tm_read_i2(uint16_t *addr)
{
	return swisstm::tm_read_i2(addr);
}

static uint32_t real_tm_read_i4(uint32_t *addr)
{
	return swisstm::tm_read_i4(addr);
}

static uint64_t real_tm_read_i8(uint64_t *addr)
{
	return swisstm::tm_read_i8(addr);
}

static float real_tm_read_f4(float *addr) { return swisstm::tm_read_f4(addr); }

static double real_tm_read_f8(double *addr) { return swisstm::tm_read_f8(addr); }

static void *real_tm_read_ptr(void **addr) { return swisstm::tm_read_ptr(addr); }

static void real_tm_write_i1(uint8_t *addr, uint8_t val)
{
	swisstm::tm_write_i1(addr, val);
}

static void real_tm_write_i2(uint16_t *addr, uint16_t val)
{
	swisstm::tm_write_i2(addr, val);
}

static void real_tm_write_i4(uint32_t *addr, uint32_t val)
{
	swisstm::tm_write_i4(addr, val);
}

static void real_tm_write_i8(uint64_t *addr, int64_t val)
{
	swisstm::tm_write_i8(addr, val);
}

static void real_tm_write_f4(float *addr, float val)
{
	swisstm::tm_write_f4(addr, val);
}

static void real_tm_write_f8(double *addr, double val)
{
	swisstm::tm_write_f8(addr, val);
}

static void real_tm_write_ptr(void **addr, void *val)
{
	swisstm::tm_write_ptr(addr, val);
}

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

void tm_read_i16(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    out_words[0] = swisstm::tm_read_i8(static_cast<uint64_t *>(addr) + 0);
    out_words[1] = swisstm::tm_read_i8(static_cast<uint64_t *>(addr) + 1);
}

void tm_read_i32(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    for (int i = 0; i < 4; i++)
        out_words[i] = swisstm::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}

void tm_read_i64(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    for (int i = 0; i < 8; i++)
        out_words[i] = swisstm::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}

void *tm_read_z(uint8_t *addr, uint64_t len)
{
	assert(len < TM_BUFFER_SIZE);
	for (uint64_t i = 0; i < len; i++) {
		tm_buffer[i] = swisstm::tm_read_i1(&addr[i]);
	}
	return tm_buffer;
}

void tm_write_i16(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 2; i++)
        swisstm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_i32(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 4; i++)
        swisstm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_i64(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    for (int i = 0; i < 8; i++)
        swisstm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len)
{
	for (uint64_t i = 0; i < len; i++) {
		swisstm::tm_write_i1(&dst[i], src[i]);
	}
}

void tm_memset(uint8_t *addr, uint8_t val, uint64_t len)
{
	for (uint64_t i = 0; i < len; i++) {
		swisstm::tm_write_i1(&addr[i], val);
	}
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {}

} // extern "C"

static void print_stats()
{
#ifndef NDEBUG
	fprintf(stderr, "=== SwissTM_new Runtime Stats ===\n");
	fprintf(stderr,
	        "tm_begin: %lld, tm_end: %lld\n",
	        (long long)g_tm_begin_count.load(std::memory_order_relaxed),
	        (long long)g_tm_end_count.load(std::memory_order_relaxed));
#endif
}

static int init = (std::atexit(print_stats), 0);

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

const TMRealHooks g_swisstm_hooks = {
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
};
