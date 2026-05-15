#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "NOrec_globals.hpp"
#include "../tm_alloc_overrides.hpp"
thread_local bool g_in_tx = false;

extern "C" {
// Thread-local state
static __thread int8_t tm_is_init_ready = 0;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
// sigjmp_buf is typically ~200 bytes, use 256 to be safe
__thread sigjmp_buf tm_jmpbuf;

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

void tm_init() { norec::init(); }

void tm_exit() { norec::exit(); }

void tm_init_thread()
{
	norec::init_thread();
	// Set TinySTM's jmpbuf pointer to our thread-local buffer
	norec::jmpbuf = &tm_jmpbuf;
	// Set up the jump point for transaction aborts
	sigsetjmp(tm_jmpbuf, 0);
}

void tm_exit_thread() { norec::exit_thread(); }

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }

void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp()
{
	// This should NOT be called - the plugin injects setjmp
	// Just return 0 to satisfy the linker
	return 0;
}

void tm_set_jmpbuf(void *buf) { }

sigjmp_buf *tm_get_env() { return &tm_jmpbuf; }

void tm_set_env(sigjmp_buf *env)
{
	if (env) {
		memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
		norec::jmpbuf = &tm_jmpbuf;
		sigsetjmp(tm_jmpbuf, 0);
	}
}

// Wrapper functions matching plugin interface

void tm_begin()
{
	// Always print to see if called
	fprintf(stderr, "NOrec tm_begin called\n");
	if (tm_longjmp_ret == 0)
		norec::begin();
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

void tm_end()
{
	fprintf(stderr, "NOrec tm_end called\n");
	norec::commit();
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
	g_tm_tx_count.fetch_add(1, std::memory_order_relaxed);
}

// Read wrappers
uint8_t tm_read_i1(uint8_t *addr, uint32_t symbol_id) { return norec::tm_read_i1(addr); }

uint16_t tm_read_i2(uint16_t *addr, uint32_t symbol_id)
{
	return norec::tm_read_i2(addr);
}

uint32_t tm_read_i4(uint32_t *addr, uint32_t symbol_id)
{
	return norec::tm_read_i4(addr);
}

uint64_t tm_read_i8(uint64_t *addr, uint32_t symbol_id)
{
	return norec::tm_read_i8(addr);
}

float tm_read_f4(float *addr, uint32_t symbol_id) { return norec::tm_read_f4(addr); }

double tm_read_f8(double *addr, uint32_t symbol_id) { return norec::tm_read_f8(addr); }

void *tm_read_ptr(void **addr, uint32_t symbol_id) { return norec::tm_read_ptr(addr); }

void *tm_read_z(uint8_t *addr, uint64_t len, uint32_t symbol_id)
{
	assert(len < TM_BUFFER_SIZE);
	for (uint64_t i = 0; i < len / 8; i++) {
		tm_buffer[i] = norec::tm_read_i8(((uint64_t *)addr) + i);
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) { // rem > 0
		tm_buffer[i] = norec::tm_read_i1(addr + (len - rem - 1) + i);
	}
	return tm_buffer;
}

// Write wrappers
void tm_write_i1(uint8_t *addr, uint8_t val, uint32_t symbol_id)
{
	norec::tm_write_i1(addr, val);
}

void tm_write_i2(uint16_t *addr, uint16_t val, uint32_t symbol_id)
{
	norec::tm_write_i2(addr, val);
}

void tm_write_i4(uint32_t *addr, uint32_t val, uint32_t symbol_id)
{
	norec::tm_write_i4(addr, val);
}

void tm_write_i8(uint64_t *addr, uint64_t val, uint32_t symbol_id)
{
	norec::tm_write_i8(addr, val);
}

void tm_write_f4(float *addr, float val, uint32_t symbol_id)
{
	norec::tm_write_f4(addr, val);
}

void tm_write_f8(double *addr, double val, uint32_t symbol_id)
{
	norec::tm_write_f8(addr, val);
}

void tm_write_ptr(void **addr, void *val, uint32_t symbol_id)
{
	norec::tm_write_ptr(addr, val);
}

void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len, uint32_t symbol_id)
{
	for (uint64_t i = 0; i < len / 8; i++) {
		norec::tm_write_i8(((uint64_t *)dst) + i, *(((uint64_t *)src) + i));
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) { // rem > 0
		norec::tm_write_i1(dst + (len - rem - 1) + i, *(src + (len - rem - 1) + i));
	}
}

void tm_memset(uint8_t *addr, uint8_t val, uint64_t len, uint32_t symbol_id)
{
	for (uint64_t i = 0; i < len; i++) {
		norec::tm_write_i1(&addr[i], val);
	}
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {}

static void print_stats()
{
	fprintf(stderr, "=== NOrec Runtime Stats ===\n");
	fprintf(stderr,
	        "tm_begin: %lld, tm_end: %lld, #TXs: %lld\n",
	        (long long)g_tm_begin_count.load(std::memory_order_relaxed),
	        (long long)g_tm_end_count.load(std::memory_order_relaxed),
	        (long long)g_tm_tx_count.load(std::memory_order_relaxed));
}

static int init = (std::atexit(print_stats), 0);

// TM allocator stubs (redirect to system allocator)
void* tm_malloc(size_t size) { return g_in_tx ? malloc(size) : malloc(size); }
void* tm_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void* tm_realloc(void* ptr, size_t size) { return realloc(ptr, size); }
void  tm_free(void* ptr) { free(ptr); }

} // extern "C"
