// AB-BA lock-ordering stress (review-05 G-05).
//
// Half the threads lock A-then-B, half B-then-A inside transactions.
// A backend with unbounded spin and no cycle-breaking hangs here (the
// pre-fix GAccO CPU fallback did); correct backends detect the cycle
// (bounded spin / abort-on-contention), roll back, and restart.
// Exact final counters prove nothing was lost or double-applied across
// all those restarts.
#include "test_helpers.hpp"

#include <thread>
#include <vector>

static constexpr int NUM_THREADS = 8;
static constexpr int ITERATIONS = 500;

int main()
{
	printf("AB-BA deadlock stress — opposite lock orders must not hang\n");
	printf("Threads: %d  Iterations: %d\n\n", NUM_THREADS, ITERATIONS);

	tm_init();
	tm_set_num_threads(NUM_THREADS);
	tm_init_thread();
	tm_nested_call_counter++;

	// 64-byte apart so no lock-table slot aliases the two cells together
	volatile uint64_t *cells = (volatile uint64_t *)tm_malloc(256);
	volatile uint64_t *a = cells;
	volatile uint64_t *b = cells + 8;
	*a = 0;
	*b = 0;

	auto worker = [&](int tid) {
		tm_init_thread();
		tm_nested_call_counter++;
		for (int i = 0; i < ITERATIONS; ++i) {
			tm_transaction([&]() {
				if (tid & 1) {
					tm_w8((uint64_t *)a, tm_r8((uint64_t *)a) + 1);
					tm_w8((uint64_t *)b, tm_r8((uint64_t *)b) + 1);
				} else {
					tm_w8((uint64_t *)b, tm_r8((uint64_t *)b) + 1);
					tm_w8((uint64_t *)a, tm_r8((uint64_t *)a) + 1);
				}
			});
		}
		tm_nested_call_counter--;
		tm_exit_thread();
	};

	std::vector<std::thread> ts;
	for (int t = 0; t < NUM_THREADS; t++)
		ts.emplace_back(worker, t);
	for (auto &th : ts)
		th.join();

	tm_nested_call_counter--;
	tm_exit_thread();
	tm_exit();

	long expected = (long)NUM_THREADS * ITERATIONS;
	int ok_a = (long)*a == expected, ok_b = (long)*b == expected;
	printf("  A: %s (got %ld, expected %ld)\n",
	       ok_a ? "PASS" : "FAIL",
	       (long)*a,
	       expected);
	printf("  B: %s (got %ld, expected %ld)\n",
	       ok_b ? "PASS" : "FAIL",
	       (long)*b,
	       expected);
	if (!(ok_a && ok_b)) {
		printf("Result: FAIL\n");
		return 1;
	}
	printf("Result: PASS\n");
	return 0;
}
