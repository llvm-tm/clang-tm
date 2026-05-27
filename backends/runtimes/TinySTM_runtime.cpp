/**
 * TinySTM Runtime Wrapper for LLVM TM Plugin
 * Uses TinySTM for transactional memory
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
#include <thread>

#include "tinystm_globals.hpp"
#include "../tm_alloc_overrides.hpp"
thread_local bool g_in_tx = false;
thread_local FreeNode* g_deferred_frees = nullptr;
thread_local SpecAlloc* g_spec_allocs = nullptr;

extern "C" {

__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
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
thread_local uint64_t g_tm_tx_read_set{0};
thread_local uint64_t g_tm_tx_write_set{0};
static std::atomic<uint64_t> g_tm_begin_count{0};
static std::atomic<uint64_t> g_tm_end_count{0};
static std::atomic<uint64_t> g_tm_tx_count{0};
void tm_init() {
	tinystm::init();
	tinystm::reset_locks();
}

void tm_exit() {
	tinystm::exit();
#ifndef NDEBUG
	fprintf(stderr, "\n=== TinySTM max read-set = %llu, max write-set = %llu ===\n",
		(unsigned long long)g_tm_max_read_set.load(),
		(unsigned long long)g_tm_max_write_set.load());
#endif
	if (auto ac = tinystm::g_tm_abort_count.load(); ac > 0) {
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
}

void tm_exit_thread()
{
	// no-op — Transaction object persists for the thread's lifetime
}

// Serialization lock for functions that couldn't be cloned.
// Using recursive_mutex so the same thread can re-acquire after a longjmp retry.
static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock()
{
	g_serialize_mutex.lock();
}

void tm_serialize_unlock()
{
	g_serialize_mutex.unlock();
}

void tm_set_jmpbuf(void *buf) { tinystm::jmpbuf = (sigjmp_buf *)buf; }

int tm_setjmp() { return 0; }

void tm_begin()
{
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
	tm_begin_count++;
	if (tm_nested_call_counter == 1) { g_in_tx = true;
		tinystm::jmpbuf = (sigjmp_buf *)&tm_jmpbuf;
		tm_clear_spec_allocs();
		tm_clear_deferred_frees();
		tinystm::begin();
	}
	assert(tm_nested_call_counter >= 0);
}

void tm_end()
{
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
	tm_end_count++;
	if (tm_nested_call_counter == 1) { g_in_tx = false;
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
            (void)0;
		}
		tinystm::commit();
		tm_flush_spec_allocs();
		tm_flush_deferred_frees();
	}
	assert(tm_nested_call_counter >= 0);
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
void* tm_malloc(size_t size) { void* p = ::operator new(size); tm_track_spec_alloc(p); return p; }
void* tm_calloc(size_t nmemb, size_t size) { void* p = ::operator new(nmemb * size); memset(p, 0, nmemb * size); tm_track_spec_alloc(p); return p; }
void* tm_realloc(void* ptr, size_t size) { void* p = ::operator new(size); if (ptr) { memcpy(p, ptr, size); ::operator delete(ptr); } tm_track_spec_alloc(p); return p; }
void  tm_free(void* ptr) {
	if (g_in_tx) {
		tm_untrack_spec_alloc(ptr);
		auto* node = static_cast<FreeNode*>(::operator new(sizeof(FreeNode)));
		node->ptr = ptr;
		node->next = g_deferred_frees;
		g_deferred_frees = node;
	} else {
		::operator delete(ptr);
	}
}

}
