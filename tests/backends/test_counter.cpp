#include "test_api.hpp"
#include "test_harness.hpp"

static volatile uint64_t g_counter;

static constexpr int NUM_THREADS = 8;
static constexpr int ITERS_PER_THREAD = 10000;

static void thread_func(int)
{
	tm_init_thread();
	tx_loop(ITERS_PER_THREAD, [&]() {
		uint64_t v = tm_test_read_i8((uint64_t *)&g_counter);
		tm_test_write_i8((uint64_t *)&g_counter, v + 1);
	});
	tm_exit_thread();
}

int main()
{
	fprintf(stderr, "\n=== Counter Test: %d threads x %d iterations ===\n\n",
	        NUM_THREADS, ITERS_PER_THREAD);

	tm_init();

	g_counter = 0;

	Timer timer;
	std::vector<std::thread> threads;
	for (int i = 0; i < NUM_THREADS; i++)
		threads.emplace_back(thread_func, i);
	for (auto &t : threads)
		t.join();

	int ms = (int)timer.elapsed_ms();

	tm_exit();

	uint64_t expected = (uint64_t)NUM_THREADS * ITERS_PER_THREAD;
	fprintf(stderr, "\nCounter: %lu (expected %lu) in %d ms\n",
	        (unsigned long)g_counter, (unsigned long)expected, ms);

	if (g_counter == expected) {
		fprintf(stderr, "TEST PASSED\n\n");
		return 0;
	} else {
		fprintf(stderr, "TEST FAILED\n\n");
		return 1;
	}
}
