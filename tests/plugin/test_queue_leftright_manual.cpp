/// Test: LeftRight backend + tm-instrument-queue-manual pipeline
/// Verifies barriers are skipped when g_tm_queue_active is non-zero
/// (only worker threads run TM transactions; main thread would never
/// reach the barrier).

#include <cstdio>

extern "C" void (*tm_wait_prev_tx)(void);

static int shared = 0;

__attribute__((noinline, annotate("async_shared"))) void increment(int delta)
{
	shared += delta;
}

__attribute__((noinline, annotate("async_shared"))) void async_increment(int delta)
{
	shared += delta;
}

int main()
{
	increment(42);
	async_increment(58);
	tm_wait_prev_tx();

	int expected = 42 + 58;
	if (shared == expected) {
		printf("PASS\n");
		return 0;
	} else {
		printf("FAIL: shared=%d expected=%d\n", shared, expected);
		return 1;
	}
}
