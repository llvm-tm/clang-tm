// This is what the plugin produces for retry.cpp
// Instrumented version of: test/retry.cpp

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

extern "C" {
extern __thread unsigned char tm_jmpbuf[256];
extern __thread int32_t tm_longjmp_ret;
extern __thread int32_t tm_nested_call_counter;
}

TM int32_t tm_counter = 0;
TM int32_t tm_max_retries = 3;

__thread int32_t tm_nested_call_counter_impl = 0;
__thread unsigned char tm_jmpbuf_impl[256];
__thread int32_t tm_longjmp_ret_impl = 0;

void tm_init() { printf("tm_init\n"); }
void tm_exit() { printf("tm_exit\n"); }
void tm_init_thread() { printf("tm_init_thread\n"); }
void tm_exit_thread() { printf("tm_exit_thread\n"); }

void tm_begin() { printf("tm_begin outer\n"); }
void tm_end() { printf("tm_end outer\n"); }

int32_t tm_read_i4(void *addr) { return *(int32_t *)addr; }
void tm_write_i4(void *addr, int32_t val) { *(int32_t *)addr = val; }

void retry_transaction()
{
	tm_nested_call_counter++;

	// ===== ENTRY =====
	if (tm_nested_call_counter == 1) {
		tm_longjmp_ret_impl = sigsetjmp(*(sigjmp_buf *)tm_jmpbuf, 0);
		tm_nested_call_counter = 1; // nesting becomes 1 on abort
		tm_begin();
	} else {
		printf("tm_begin nested %d\n", tm_nested_call_counter);
	}

	// ===== BODY =====
	printf("retry_transaction: start, counter=%d, jmpbuf_ret=%d, nested_counter=%d\n",
	       tm_counter,
	       tm_longjmp_ret,
	       tm_nested_call_counter);

	if (tm_longjmp_ret != 0) {
		printf("retry_transaction: retry detected! jmpbuf_ret=%d\n", tm_longjmp_ret);
		if (tm_longjmp_ret >= tm_max_retries) {
			printf("retry_transaction: ERROR - infinite loop! jmpbuf_ret=%d >= %d\n",
			       tm_longjmp_ret,
			       tm_max_retries);
		}
	}

	tm_counter++;
	printf("retry_transaction: incremented counter to %d\n", tm_counter);

	if (tm_counter < tm_max_retries) {
		printf("retry_transaction: triggering longjmp with value %d\n", tm_counter);
		longjmp(*(sigjmp_buf *)tm_jmpbuf, tm_counter);
	}

	printf("retry_transaction: committed! counter=%d\n", tm_counter);

	// ===== EXIT =====
	if (tm_nested_call_counter == 1) {
		tm_end();
	} else {
		printf("tm_end nested %d\n", tm_nested_call_counter);
	}
	tm_nested_call_counter--;
}

void outer_retry_transaction()
{
	tm_nested_call_counter++;

	if (tm_nested_call_counter == 1) {
		tm_longjmp_ret_impl = sigsetjmp(*(sigjmp_buf *)tm_jmpbuf, 0);
		tm_nested_call_counter = 1; // nesting becomes 1 on abort
		tm_begin();
	} else {
		printf("tm_begin nested %d\n", tm_nested_call_counter);
	}

	printf("outer_retry_transaction: start, counter=%d\n", tm_counter);
	retry_transaction();
	printf("outer_retry_transaction: done, counter=%d\n", tm_counter);

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
	tm_init_thread();

	printf("=== Test: Transaction retry with longjmp ===\n");

	tm_counter = 0;
	outer_retry_transaction();

	printf("main: final counter = %d (expected %d)\n", tm_counter, tm_max_retries);

	if (tm_counter != tm_max_retries) {
		printf("FAILED: expected counter=%d, got %d\n", tm_max_retries, tm_counter);
		return 1;
	}

	printf("Test PASSED!\n");
	_exit(0);
}
