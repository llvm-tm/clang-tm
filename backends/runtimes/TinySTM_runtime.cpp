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

#include "tinystm_globals.hpp"

extern "C" {

__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;
__thread unsigned char tm_jmpbuf[256];

#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];

thread_local uint64_t tm_begin_count{0};
thread_local uint64_t tm_end_count{0};
thread_local uint64_t tm_tx_count{0};

static std::atomic<uint64_t> g_tm_begin_count{0};
static std::atomic<uint64_t> g_tm_end_count{0};
static std::atomic<uint64_t> g_tm_tx_count{0};

void tm_init()
{
	tinystm::init();
}

void tm_exit()
{
	tinystm::exit();
}

void tm_init_thread()
{
	tinystm::init_thread();
}

void tm_exit_thread()
{
	// no-op — Transaction object persists for the thread's lifetime
}

void tm_set_jmpbuf(void *buf)
{
	tinystm::jmpbuf = (sigjmp_buf *)buf;
}

int tm_setjmp()
{
	return 0;
}

void tm_begin()
{
	g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
	tm_begin_count++;
	if (tm_longjmp_ret == 0) {
		tinystm::begin();
	}
}

void tm_end()
{
	g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
	tm_end_count++;
	tinystm::commit();
	tm_tx_count++;
}

uint8_t tm_read_i1(uint8_t *addr) { return tinystm::tm_read_i1(addr); }
uint16_t tm_read_i2(uint16_t *addr) { return tinystm::tm_read_i2(addr); }
uint32_t tm_read_i4(uint32_t *addr) { return tinystm::tm_read_i4(addr); }
uint64_t tm_read_i8(uint64_t *addr) { return tinystm::tm_read_i8(addr); }
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
	for (uint64_t i = 0; i < rem; i++) {
		tm_buffer[i] = tinystm::tm_read_i1(addr + (len - rem - 1) + i);
	}
	return tm_buffer;
}

void tm_write_i1(uint8_t *addr, uint8_t val) { tinystm::tm_write_i1(addr, val); }
void tm_write_i2(uint16_t *addr, uint16_t val) { tinystm::tm_write_i2(addr, val); }
void tm_write_i4(uint32_t *addr, uint32_t val) { tinystm::tm_write_i4(addr, val); }
void tm_write_i8(uint64_t *addr, uint64_t val) { tinystm::tm_write_i8(addr, val); }
void tm_write_f4(float *addr, float val) { tinystm::tm_write_f4(addr, val); }
void tm_write_f8(double *addr, double val) { tinystm::tm_write_f8(addr, val); }
void tm_write_ptr(void **addr, void *val) { tinystm::tm_write_ptr(addr, val); }

void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len)
{
	for (uint64_t i = 0; i < len / 8; i++) {
		tinystm::tm_write_i8(((uint64_t *)dst) + i, *(((uint64_t *)src) + i));
	}
	uint64_t rem = len % 8;
	for (uint64_t i = 0; i < rem; i++) {
		tinystm::tm_write_i1(dst + (len - rem - 1) + i, src[(len - rem - 1) + i]);
	}
}

void tm_memset(uint8_t *addr, uint8_t val, uint64_t len)
{
	for (uint64_t i = 0; i < len; i++) {
		tinystm::tm_write_i1(&addr[i], val);
	}
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {}
void consume_ptr(volatile void *ptr) { (void)ptr; }

}