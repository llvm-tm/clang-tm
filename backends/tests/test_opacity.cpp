#include "test_api.hpp"
#include "test_harness.hpp"
#include <cstring>
#include <atomic>

// ══════════════════════════════════════════════════════════════════════
// Section 1: Write-skew — classic SI anomaly
//
// X=1, Y=1 initially.
//   TX1: read X, read Y, if X>0 && Y>0 → set X=0
//   TX2: read X, read Y, if X>0 && Y>0 → set Y=0
//
// Without opacity (write-skew), both TXes can see X>0 && Y>0 and both
// write a 0.  The final state (X=0, Y=0) is impossible in any serial
// execution:
//   serial(TX1,TX2): TX1 sees X=1,Y=1 → sets X=0; TX2 sees X=0 → aborts
//   serial(TX2,TX1): TX2 sees X=1,Y=1 → sets Y=0; TX1 sees Y=0 → aborts
//
// Correct TM: exactly one of X,Y is 0 after both TXes run once.
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_ws_x;
static volatile uint64_t g_ws_y;

static void thread_ws_set_x(int)
{
	tm_init_thread();
	tx_loop(1, [&]() {
		uint64_t x = tm_test_read_i8((uint64_t *)&g_ws_x);
		uint64_t y = tm_test_read_i8((uint64_t *)&g_ws_y);
		if (x > 0 && y > 0)
			tm_test_write_i8((uint64_t *)&g_ws_x, 0);
	});
	tm_exit_thread();
}

static void thread_ws_set_y(int)
{
	tm_init_thread();
	tx_loop(1, [&]() {
		uint64_t x = tm_test_read_i8((uint64_t *)&g_ws_x);
		uint64_t y = tm_test_read_i8((uint64_t *)&g_ws_y);
		if (x > 0 && y > 0)
			tm_test_write_i8((uint64_t *)&g_ws_y, 0);
	});
	tm_exit_thread();
}

static void test_write_skew()
{
	fprintf(stderr, "  test_write_skew ...\n");

	// Run 1000 rounds; in each round, reset X=1,Y=1 and run both TXes.
	int violations = 0;
	for (int r = 0; r < 1000; r++) {
		g_ws_x = 1;
		g_ws_y = 1;

		std::thread t1(thread_ws_set_x, 0);
		std::thread t2(thread_ws_set_y, 0);
		t1.join();
		t2.join();

		if (g_ws_x == 0 && g_ws_y == 0) {
			violations++;
			if (violations <= 3)
				fprintf(stderr, "  round %d: X=0, Y=0 (write-skew)\n", r);
		}
	}

	if (violations > 0)
		fprintf(stderr, "  OPACITY VIOLATION: %d/1000 write-skew rounds\n", violations);
	TEST_ASSERT(violations == 0, "write-skew: no round has both X=0 and Y=0");
}

// ══════════════════════════════════════════════════════════════════════
// Section 2: Read-set validation under contention
//
// TX1 reads X and writes Y = f(X).  TX2 writes X simultaneously.
// At commit, TX1 must detect X's version change and abort.
// Without proper read-set validation, TX1 could commit a stale Y.
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_rv_x;
static volatile uint64_t g_rv_y;
static constexpr int RV_ITERS = 3000;

static void thread_rv_reader(int)
{
	tm_init_thread();
	tx_loop(RV_ITERS, [&]() {
		uint64_t x = tm_test_read_i8((uint64_t *)&g_rv_x);
		tm_test_write_i8((uint64_t *)&g_rv_y, x * 2);
		tm_test_write_i8((uint64_t *)&g_rv_x, x + 1);
	});
	tm_exit_thread();
}

static void thread_rv_writer(int)
{
	tm_init_thread();
	tx_loop(RV_ITERS, [&]() {
		uint64_t x = tm_test_read_i8((uint64_t *)&g_rv_x);
		tm_test_write_i8((uint64_t *)&g_rv_x, x + 1);
	});
	tm_exit_thread();
}

static void test_read_validation()
{
	fprintf(stderr, "  test_read_validation ...\n");
	g_rv_x = 0;
	g_rv_y = 0;

	std::thread t1(thread_rv_reader, 0);
	std::thread t2(thread_rv_writer, 0);
	t1.join();
	t2.join();

	uint64_t final_x = g_rv_x;
	uint64_t final_y = g_rv_y;
	uint64_t max_x = 2 * RV_ITERS;
	fprintf(stderr, "  Final X=%lu Y=%lu (max X: %lu)\n",
	        (unsigned long)final_x, (unsigned long)final_y,
	        (unsigned long)max_x);
	TEST_ASSERT(final_x <= max_x, "X in range");
	TEST_ASSERT(final_y <= final_x * 2, "Y consistent with X");
}

// ══════════════════════════════════════════════════════════════════════
// Section 3: High-contention read-set validation
//
// 8 threads all read X and write X+1.  Each TX reads X then
// immediately writes it back incremented.  Every read should
// match the value that was just read.  Since each TX writes
// what it read plus 1, the final value equals the total
// number of committed increments.
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_hc_x;
static constexpr int HC_ITERS = 2000;

static void thread_hc(int)
{
	tm_init_thread();
	tx_loop(HC_ITERS, [&]() {
		uint64_t x = tm_test_read_i8((uint64_t *)&g_hc_x);
		tm_test_write_i8((uint64_t *)&g_hc_x, x + 1);
	});
	tm_exit_thread();
}

static void test_high_contention()
{
	fprintf(stderr, "  test_high_contention ...\n");
	g_hc_x = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 8; i++)
		threads.emplace_back(thread_hc, i);
	for (auto &t : threads)
		t.join();

	uint64_t expected = (uint64_t)(8 * HC_ITERS);
	TEST_ASSERT_EQ(g_hc_x, expected, "hc_x == 8*2000");
}

// ══════════════════════════════════════════════════════════════════════
// ── Main ──
// ══════════════════════════════════════════════════════════════════════

int main()
{
	fprintf(stderr, "\n=== Opacity Tests ===\n\n");
	tm_init();

	test_write_skew();
	test_read_validation();
	test_high_contention();

	tm_exit();

	fprintf(stderr, "\n=== Results: %d failures ===\n\n", g_test_failures);
	return g_test_failures;
}
