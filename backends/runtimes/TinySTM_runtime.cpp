/**
 * TinySTM Runtime Wrapper for LLVM TM Plugin
 * Uses TinySTM for transactional memory
 */

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

// either #define DESIGN_WBCTL or pass -D in the compiler

// #if defined(DESIGN_WBETL)
// #include "tinystm_wbetl.hpp"
// #elif defined(DESIGN_WBCTL)
// #include "tinystm_wbctl.hpp"
// #elif defined(DESIGN_WT)
// #include "tinystm_wt.hpp"
// #else
// #pragma error("Define one of the following: DESIGN_WBETL, DESIGN_WBCTL, DESIGN_WT")
// #endif

#include "tinystm_globals.hpp"

extern "C" {
// Thread-local state
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;
// sigjmp_buf is typically ~200 bytes, use 256 to be safe
__thread sigjmp_buf tm_jmpbuf;

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

// Global transaction counters
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};

void tm_init() { tinystm::init(); }

void tm_exit() { tinystm::exit(); }

void tm_init_thread()
{
	tinystm::init_thread();
	tinystm::setjmp(&tm_jmpbuf);
}

void tm_exit_thread() { tinystm::exit_thread(); }

int tm_setjmp()
{
	// This should NOT be called - the plugin injects setjmp
	// Just return 0 to satisfy the linker
	return 0;
}

// sigjmp_buf *tm_get_env() { return &tm_jmpbuf; }

// void tm_set_env(sigjmp_buf *env)
// {
// 	if (env) {
// 		memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
// 		tinystm::jmpbuf = &tm_jmpbuf;
// 		sigsetjmp(tm_jmpbuf, 0);
// 	}
// }

// Wrapper functions matching plugin interface

static bool g_initialized = false;

void tm_begin()
{
	if (tm_longjmp_ret == 0) {
		tinystm::begin();
	}
}

void tm_end()
{
	if (tm_nested_call_counter == 1 && tm_longjmp_ret == 0) {
		tinystm::commit();
	}
}

// Read wrappers
uint8_t tm_read_i1(uint8_t *addr)
{
	return tinystm::tm_read_i1(addr);
}

uint16_t tm_read_i2(uint16_t *addr)
{
	return tinystm::tm_read_i2(addr);
}

uint32_t tm_read_i4(uint32_t *addr)
{
	return tinystm::tm_read_i4(addr);
}

uint64_t tm_read_i8(uint64_t *addr)
{
	return tinystm::tm_read_i8(addr);
}

float tm_read_f4(float *addr) { return tinystm::tm_read_f4(addr); }

double tm_read_f8(double *addr) { return tinystm::tm_read_f8(addr); }

void *tm_read_ptr(void **addr) { return tinystm::tm_read_ptr(addr); }

void *tm_read_z(uint8_t *addr, uint64_t len)
{
	assert(len < TM_BUFFER_SIZE);
	for (uint64_t i = 0; i < len / 8; i++) {
		tm_buffer[i] = tinystm::tm_read_i8(((uint64_t *)addr) + i);
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) { // rem > 0
		tm_buffer[i] = tinystm::tm_read_i1(addr + (len - rem - 1) + i);
	}
	return tm_buffer;
}

// Write wrappers
void tm_write_i1(uint8_t *addr, uint8_t val)
{
	tinystm::tm_write_i1(addr, val);
}

void tm_write_i2(uint16_t *addr, uint16_t val)
{
	tinystm::tm_write_i2(addr, val);
}

void tm_write_i4(uint32_t *addr, uint32_t val)
{
	tinystm::tm_write_i4(addr, val);
}

void tm_write_i8(uint64_t *addr, uint64_t val)
{
	tinystm::tm_write_i8(addr, val);
}

void tm_write_f4(float *addr, float val)
{
	tinystm::tm_write_f4(addr, val);
}

void tm_write_f8(double *addr, double val)
{
	tinystm::tm_write_f8(addr, val);
}

void tm_write_ptr(void **addr, void *val)
{
	tinystm::tm_write_ptr(addr, val);
}

void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len)
{
	for (uint64_t i = 0; i < len / 8; i++) {
		tinystm::tm_write_i8(((uint64_t *)dst) + i, *(((uint64_t *)src) + i));
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) { // rem > 0
		tinystm::tm_write_i1(dst + (len - rem - 1) + i, *(src + (len - rem - 1) + i));
	}
}

void tm_memset(uint8_t *addr, uint8_t val, uint64_t len)
{
	for (uint64_t i = 0; i < len; i++) {
		tinystm::tm_write_i1(&addr[i], val);
	}
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {}

static void print_stats()
{
	fprintf(stderr, "=== TinySTM_new Runtime Stats ===\n");
	fprintf(stderr,
	        "tm_begin: %lld, tm_end: %lld\n",
	        (long long)g_tm_begin_count.load(std::memory_order_relaxed),
	        (long long)g_tm_end_count.load(std::memory_order_relaxed));
}

static int init = (std::atexit(print_stats), 0);

void consume_ptr(volatile void *ptr) { (void)ptr; }

} // extern "C"
