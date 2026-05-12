// This is what the plugin produces for types.cpp
// Instrumented version of: test/types.cpp

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// TM globals
TM int8_t tm_i8 = 10;
TM int16_t tm_i16 = 20;
TM int32_t tm_i32 = 30;
TM int64_t tm_i64 = 40;
TM float tm_f4 = 1.5f;
TM double tm_f8 = 2.5;
TM volatile void *tm_ptr = nullptr;

// Thread-local state
__thread int32_t tm_nested_call_counter = 0;
__thread unsigned char tm_jmpbuf[256];
__thread int32_t tm_longjmp_ret = 0;
__thread uint8_t is_tm_init_thread_ready = 0;

// Runtime hooks
void tm_init() { printf("tm_init\n"); }
void tm_exit() { printf("tm_exit\n"); }
void tm_init_thread() { printf("tm_init_thread\n"); }
void tm_exit_thread() { printf("tm_exit_thread\n"); }

void tm_begin() { printf("tm_begin outer\n"); }
void tm_end() { printf("tm_end outer\n"); }

// Read instrumentation
int8_t tm_read_i1(void *addr) { return *(int8_t *)addr; }
int16_t tm_read_i2(void *addr) { return *(int16_t *)addr; }
int32_t tm_read_i4(void *addr) { return *(int32_t *)addr; }
int64_t tm_read_i8(void *addr) { return *(int64_t *)addr; }
float tm_read_f4(void *addr) { return *(float *)addr; }
double tm_read_f8(void *addr) { return *(double *)addr; }
void *tm_read_ptr(void *addr) { return *(void **)addr; }

// Write instrumentation
void tm_write_i1(void *addr, int8_t val) { *(int8_t *)addr = val; }
void tm_write_i2(void *addr, int16_t val) { *(int16_t *)addr = val; }
void tm_write_i4(void *addr, int32_t val) { *(int32_t *)addr = val; }
void tm_write_i8(void *addr, int64_t val) { *(int64_t *)addr = val; }
void tm_write_f4(void *addr, float val) { *(float *)addr = val; }
void tm_write_f8(void *addr, double val) { *(double *)addr = val; }
void tm_write_ptr(void *addr, void *val) { *(void **)addr = val; }

extern "C" void consume_ptr(volatile void *ptr);

void tm_types()
{
	tm_nested_call_counter++;

	// Entry handling
	if (tm_nested_call_counter == 1) {
		// OUTER path - first transaction entry
		tm_longjmp_ret = sigsetjmp(*(sigjmp_buf *)tm_jmpbuf, 0);
		tm_nested_call_counter = 1; // nesting becomes 1 on abort
		tm_begin();
	} else {
		printf("tm_begin nested %d\n", tm_nested_call_counter);
	}
	// NESTED path: NO tm_begin() - only outermost calls tm_begin()

	// Original function body - but with TM accesses instrumented
	{
		int8_t tmp = tm_read_i1(&tm_i8);
		tm_write_i1(&tm_i8, tmp + 1);
	}

	{
		int8_t r8 = tm_read_i1(&tm_i8);
		(void)r8;
	}

	{
		int16_t tmp = tm_read_i2(&tm_i16);
		tm_write_i2(&tm_i16, tmp + 1);
	}

	{
		int32_t tmp = tm_read_i4(&tm_i32);
		tm_write_i4(&tm_i32, tmp + 1);
	}
	int32_t r32 = tm_read_i4(&tm_i32);
	(void)r32;

	{
		int64_t tmp = tm_read_i8(&tm_i64);
		tm_write_i8(&tm_i64, tmp + 1);
	}

	// Exit handling
	if (tm_nested_call_counter == 1) {
		tm_end();
	} else {
		printf("tm_end nested %d\n", tm_nested_call_counter);
	}
	tm_nested_call_counter--;
}

MAIN int main()
{
	tm_init();
	if (is_tm_init_thread_ready == 0) {
		tm_init_thread();
		is_tm_init_thread_ready = 1;
	}
	tm_types();
	if (is_tm_init_thread_ready == 1) {
		tm_exit_thread();
		is_tm_init_thread_ready = 0;
	}
	tm_exit();
	return 0;
}
