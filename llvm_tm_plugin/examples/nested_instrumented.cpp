// This is what the plugin produces for nested.cpp
// Instrumented version of: test/nested.cpp

#include <csetjmp>
#include <cstdint>
#include <cstdio>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

// TM globals
TM int32_t tm_counter = 0;

// Thread-local state
__thread int32_t tm_nested_call_counter = 0;
__thread unsigned char tm_jmpbuf[256];
__thread int32_t tm_longjmp_ret = 0;
__thread uint8_t is_tm_init_thread_ready = 0;

// Runtime hooks (simplified)
void tm_init() { printf("tm_init\n"); }
void tm_exit() { printf("tm_exit\n"); }
void tm_init_thread() { printf("tm_init_thread\n"); }
void tm_exit_thread() { printf("tm_exit_thread\n"); }

void tm_begin() { printf("tm_begin outer\n"); }
void tm_end() { printf("tm_end outer\n"); }

int tm_read_i4(void *addr) { return *(int32_t *)addr; }
void tm_write_i4(void *addr, int32_t val) { *(int32_t *)addr = val; }

// Inner transaction function
void nested_tx()
{
	tm_nested_call_counter++;

	// ===== ENTRY =====
	if (tm_nested_call_counter == 1) {
		// OUTER path - first transaction entry
		tm_longjmp_ret = sigsetjmp(*(sigjmp_buf *)tm_jmpbuf, 0);
		tm_nested_call_counter = 1;
		tm_begin();
	} else {
		printf("tm_begin nested %d\n", tm_nested_call_counter);
	}
	// NESTED path: NO tm_begin() - only outermost calls tm_begin()

	// ===== BODY =====
	{
		int32_t tmp = tm_read_i4(&tm_counter);
		tm_write_i4(&tm_counter, tmp + 1);
	}

	// ===== EXIT =====
	if (tm_nested_call_counter == 1) {
		tm_end();
	} else {
		printf("tm_end nested %d\n", tm_nested_call_counter);
	}
	tm_nested_call_counter--;
}

// Outer transaction function
void outer_tx()
{
	tm_nested_call_counter++;

	// ===== ENTRY =====
	if (tm_nested_call_counter == 1) {
		// OUTER path - first transaction entry
		tm_longjmp_ret = sigsetjmp(*(sigjmp_buf *)tm_jmpbuf, 0);
		tm_nested_call_counter = 1; // nesting becomes 1 on abort
		tm_begin();
	} else {
		printf("tm_begin nested %d\n", tm_nested_call_counter);
	}
	// NESTED path: NO tm_begin() - only outermost calls tm_begin()

	// ===== BODY =====
	{
		int32_t tmp = tm_read_i4(&tm_counter);
		tm_write_i4(&tm_counter, tmp + 1);
	}

	// Call nested transaction (this creates nesting!)
	nested_tx();

	{
		int32_t tmp = tm_read_i4(&tm_counter);
		tm_write_i4(&tm_counter, tmp + 1);
	}

	// ===== EXIT =====
	if (tm_nested_call_counter == 1) {
		tm_end();
	} else {
		printf("tm_end nested %d\n", tm_nested_call_counter);
	}
	tm_nested_call_counter--;
}

// Main function - thread entry point
int main()
{
	tm_init();

	if (is_tm_init_thread_ready == 0) {
		tm_init_thread();
		is_tm_init_thread_ready = 1;
	}

	outer_tx();

	if (is_tm_init_thread_ready == 1) {
		tm_exit_thread();
		is_tm_init_thread_ready = 0;
	}

	tm_exit();
	return 0;
}
