/**
 * NV-HTM Runtime Wrapper for LLVM TM Plugin
 *
 * Uses Intel RTM + redo log for hardware-transactional persistent memory.
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
#include "nvhtm/nvhtm_globals.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_hooks.hpp"

thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;

extern const TMRealHooks g_nvhtm_hooks;

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

void tm_init()
{
	tm_register_real_hooks(&g_nvhtm_hooks);
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed — TM address space unavailable\n");
        std::abort();
    }
	nvhtm::init();
}

void tm_exit()
{
	nvhtm::exit();
    stm::tm_region_destroy();
}

void tm_init_thread()
{
	tm_hook_init_thread();
	nvhtm::init_thread();
	nvhtm::jmpbuf = &tm_jmpbuf;
	sigsetjmp(tm_jmpbuf, 0);
}

void tm_exit_thread()
{
	tm_hook_exit_thread();
	nvhtm::exit_thread();
}

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock()
{
	g_serialize_mutex.lock();
}

void tm_serialize_unlock()
{
	g_serialize_mutex.unlock();
}

void tm_set_jmpbuf(void *buf)
{
	nvhtm::jmpbuf = (sigjmp_buf *)buf;
}

int tm_setjmp()
{
	return 0;
}

} // extern "C" — infrastructure

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void real_tm_begin()
{
	tm_clear_spec_allocs();
	tm_clear_deferred_frees();
	g_in_tx = true;
	nvhtm::begin();
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

static void real_tm_end()
{
	nvhtm::commit();
	g_in_tx = false;
	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
}

// ---- Read hooks ----

static uint8_t real_tm_read_i1(uint8_t *addr)  { return nvhtm::tm_read_i1(addr); }
static uint16_t real_tm_read_i2(uint16_t *addr){ return nvhtm::tm_read_i2(addr); }
static uint32_t real_tm_read_i4(uint32_t *addr){ return nvhtm::tm_read_i4(addr); }
static uint64_t real_tm_read_i8(uint64_t *addr){ return nvhtm::tm_read_i8(addr); }

static float real_tm_read_f4(float *addr)    { return nvhtm::tm_read_f4(addr); }
static double real_tm_read_f8(double *addr)  { return nvhtm::tm_read_f8(addr); }
static void * real_tm_read_ptr(void **addr)  { return nvhtm::tm_read_ptr(addr); }

// ---- Write hooks ----

static void real_tm_write_i1(uint8_t *addr, uint8_t val)        { nvhtm::tm_write_i1(addr, val); }
static void real_tm_write_i2(uint16_t *addr, uint16_t val)      { nvhtm::tm_write_i2(addr, val); }
static void real_tm_write_i4(uint32_t *addr, uint32_t val)      { nvhtm::tm_write_i4(addr, val); }
static void real_tm_write_i8(uint64_t *addr, int64_t val)       { nvhtm::tm_write_i8(addr, (uint64_t)val); }

static void real_tm_write_f4(float *addr, float val)             { nvhtm::tm_write_f4(addr, val); }
static void real_tm_write_f8(double *addr, double val)           { nvhtm::tm_write_f8(addr, val); }
static void real_tm_write_ptr(void **addr, void *val)            { nvhtm::tm_write_ptr(addr, val); }

static void *real_tm_malloc(size_t size)
{
	void *p = stm::tm_region_malloc(size);
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
	if (ptr) {
		memcpy(p, ptr, size);
		stm::tm_region_free(ptr);
	}
	tm_track_spec_alloc(p);
	return p;
}

static void real_tm_free(void *ptr)
{
	if (!ptr) return;
	if (!stm::isTMAddress(ptr)) return;
	if (g_in_tx) {
		nvhtm::tm_write_i1(reinterpret_cast<uint8_t *>(ptr), 0);
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

const TMRealHooks g_nvhtm_hooks = {
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

// ═══════════════════════════════════════════════════════════════════
//  Plugin-specific extern "C" functions (not hook candidates)
// ═══════════════════════════════════════════════════════════════════

extern "C" {

void tm_read_i16(void *addr, void *out)
{
	auto *out_words = static_cast<uint64_t *>(out);
	out_words[0] = nvhtm::tm_read_i8(static_cast<uint64_t *>(addr) + 0);
	out_words[1] = nvhtm::tm_read_i8(static_cast<uint64_t *>(addr) + 1);
}

void tm_read_i32(void *addr, void *out)
{
	auto *out_words = static_cast<uint64_t *>(out);
	for (int i = 0; i < 4; i++)
		out_words[i] = nvhtm::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}

void tm_read_i64(void *addr, void *out)
{
	auto *out_words = static_cast<uint64_t *>(out);
	for (int i = 0; i < 8; i++)
		out_words[i] = nvhtm::tm_read_i8(static_cast<uint64_t *>(addr) + i);
}

void *tm_read_z(uint8_t *addr, uint64_t len)
{
	assert(len < TM_BUFFER_SIZE);
	for (uint64_t i = 0; i < len; i++)
		tm_buffer[i] = nvhtm::tm_read_i1(&addr[i]);
	return tm_buffer;
}

void tm_write_i16(void *addr, void *val)
{
	auto *val_words = static_cast<const uint64_t *>(val);
	for (int i = 0; i < 2; i++)
		nvhtm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_i32(void *addr, void *val)
{
	auto *val_words = static_cast<const uint64_t *>(val);
	for (int i = 0; i < 4; i++)
		nvhtm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_i64(void *addr, void *val)
{
	auto *val_words = static_cast<const uint64_t *>(val);
	for (int i = 0; i < 8; i++)
		nvhtm::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]);
}

void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len)
{
	for (uint64_t i = 0; i < len; i++)
		nvhtm::tm_write_i1(&dst[i], src[i]);
}

void tm_memset(uint8_t *addr, uint8_t val, uint64_t len)
{
	for (uint64_t i = 0; i < len; i++)
		nvhtm::tm_write_i1(&addr[i], val);
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) { (void)symbol_table; (void)symbol_count; }
void consume_ptr(volatile void *ptr) { (void)ptr; }

} // extern "C" — plugin-specific
