#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <mutex>
#include <unistd.h>
#include <unordered_set>

#include "../SwissTM/SwissTM.hpp"
#include "../tm_alloc_overrides.hpp"

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
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
__thread sigjmp_buf tm_jmpbuf;

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};

void tm_init()
{
	swisstm::init();
}

void tm_exit() {}

void tm_dbg_set_counter_ptr(const volatile uint64_t *p) {
    swisstm::dbg_set_counter_ptr(p);
}

void tm_init_thread()
{
	swisstm::init_thread();
}

void tm_exit_thread() { swisstm::exit_thread(); }

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

// Wrapper functions matching plugin interface

void tm_begin()
{
	swisstm::set_jmpbuf(&tm_jmpbuf);
	tm_clear_spec_allocs();
	tm_clear_deferred_frees();
	g_in_tx = true;
	swisstm::begin();
	assert(tm_nested_call_counter >= 0);
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

void tm_end()
{
	swisstm::commit();
	g_in_tx = false;
	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
	assert(tm_nested_call_counter >= 0);
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
}

uint8_t tm_read_i1(uint8_t *addr)
{
	return swisstm::tm_read_i1(addr);
}

uint16_t tm_read_i2(uint16_t *addr)
{
	return swisstm::tm_read_i2(addr);
}

uint32_t tm_read_i4(uint32_t *addr)
{
	return swisstm::tm_read_i4(addr);
}

uint64_t tm_read_i8(uint64_t *addr)
{
	return swisstm::tm_read_i8(addr);
}

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

float tm_read_f4(float *addr) { return swisstm::tm_read_f4(addr); }

double tm_read_f8(double *addr) { return swisstm::tm_read_f8(addr); }

void *tm_read_ptr(void **addr) { return swisstm::tm_read_ptr(addr); }

void *tm_read_z(uint8_t *addr, uint64_t len)
{
	assert(len < TM_BUFFER_SIZE);
	for (uint64_t i = 0; i < len; i++) {
		tm_buffer[i] = swisstm::tm_read_i1(&addr[i]);
	}
	return tm_buffer;
}

void tm_write_i1(uint8_t *addr, uint8_t val)
{
	swisstm::tm_write_i1(addr, val);
}

void tm_write_i2(uint16_t *addr, uint16_t val)
{
	swisstm::tm_write_i2(addr, val);
}

void tm_write_i4(uint32_t *addr, uint32_t val)
{
	swisstm::tm_write_i4(addr, val);
}

void tm_write_i8(uint64_t *addr, uint64_t val)
{
	swisstm::tm_write_i8(addr, val);
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

void tm_write_f4(float *addr, float val)
{
	swisstm::tm_write_f4(addr, val);
}

void tm_write_f8(double *addr, double val)
{
	swisstm::tm_write_f8(addr, val);
}

void tm_write_ptr(void **addr, void *val)
{
	swisstm::tm_write_ptr(addr, val);
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
void* tm_malloc(size_t size) { return tm_track_alloc_result(::operator new(size), size); }
void* tm_calloc(size_t nmemb, size_t size) { void* p = ::operator new(nmemb * size); memset(p, 0, nmemb * size); return tm_track_alloc_result(p, nmemb * size); }
void* tm_realloc(void* ptr, size_t size) { void* p = ::operator new(size); if (ptr) { memcpy(p, ptr, size); ::operator delete(ptr); } return tm_track_alloc_result(p, size); }
void  tm_free(void* ptr) {
	if (!ptr || stm::isStackAddress(ptr)) return;
	TM_EVENT(FREE, ptr, 0);
	if (g_in_tx) {
		swisstm::tm_write_i1(reinterpret_cast<uint8_t*>(ptr), 0);
		tm_free_append_deferred(ptr);
	} else {
		::operator delete(ptr);
	}
}

// ── Debug hooks ──────────────────────────────────────────────
#ifndef NDEBUG
#include "backends/tm_debug.hpp"
void tm_dbg_set_counter_ptr(const volatile uint64_t *p) { swisstm::g_dbg_pcounter = p; }
#endif
