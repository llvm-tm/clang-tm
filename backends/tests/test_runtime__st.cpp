/**
 * Simple single-threaded test for debugging
 */
#include <csetjmp>
#include <cstdint>
#include <cstdio>

// Runtime stubs
extern "C" {
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
void tm_init();
void tm_exit();
void tm_init_thread();
void tm_exit_thread();
void tm_begin();
void tm_end();
uint32_t tm_read_i4(volatile uint32_t *addr);
void tm_write_i4(volatile uint32_t *addr, uint32_t val);
}

volatile uint32_t counter = 0;

int main()
{
	printf("Single-threaded runtime test\n");

	tm_init();
	tm_init_thread();

	int iterations = 100;
	for (int i = 0; i < iterations; i++) {
		tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
		tm_begin();
		if (!tm_longjmp_ret)
			printf("A retry from TX %i\n", i);

		uint32_t val = tm_read_i4(&counter);
		val++;
		tm_write_i4(&counter, val);

		tm_end();
	}

	printf("Final counter: %u (expected: %d)\n", counter, iterations);

	tm_exit_thread();
	tm_exit();

	if (counter == iterations) {
		printf("TEST PASSED\n");
		return 0;
	} else {
		printf("TEST FAILED\n");
		return 1;
	}
}
