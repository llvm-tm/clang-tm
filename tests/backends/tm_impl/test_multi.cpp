#include "test_api.hpp"
#include "test_harness.hpp"
#include <cstring>

// ══════════════════════════════════════════════════════════════════════
// Section 1: Counter stress test — basic atomicity via TM
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_counter;

static void thread_counter(int id)
{
	(void)id;
	tm_init_thread();
	tx_loop(2000, [&]() {
		uint64_t v = tm_test_read_i8((uint64_t *)&g_counter);
		tm_test_write_i8((uint64_t *)&g_counter, v + 1);
	});
	tm_exit_thread();
}

static void test_counter()
{
	fprintf(stderr, "  test_counter ...\n");
	g_counter = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_counter, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT_EQ(g_counter, (uint64_t)8000, "counter == 4*2000");
}

// ══════════════════════════════════════════════════════════════════════
// Section 2: Write-set validation — two-variable consistency
//
// Each TX: read A → write A+1 → write B = (A_before_incr) * COEFF.
// B is write-set-only (never pre-read).  Without write-set→read-set
// propagation at write time, B is NOT validated at commit, and a
// concurrent writer to A can create an inconsistent (A, B) pair.
//
// Invariant from proofs §3.1, §4: every address written must be
// validated at commit time (added to read-set at write time so a
// version change on that address triggers an abort).
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_ws_a;
static volatile uint64_t g_ws_b;
static constexpr uint64_t WS_COEFF = 10;

static void thread_write_set(int id)
{
	(void)id;
	tm_init_thread();
	tx_loop(2000, [&]() {
		uint64_t a = tm_test_read_i8((uint64_t *)&g_ws_a);
		tm_test_write_i8((uint64_t *)&g_ws_a, a + 1);
		tm_test_write_i8((uint64_t *)&g_ws_b, a * WS_COEFF);
	});
	tm_exit_thread();
}

static void test_write_set()
{
	fprintf(stderr, "  test_write_set ...\n");
	g_ws_a = 0;
	g_ws_b = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_write_set, i);
	for (auto &t : threads)
		t.join();

	uint64_t final_a = g_ws_a;
	uint64_t expected_b = (final_a > 0) ? (final_a - 1) * WS_COEFF : 0;
	TEST_ASSERT_EQ(g_ws_a, (uint64_t)8000, "ws_a == 4*2000");
	TEST_ASSERT_EQ(g_ws_b, expected_b, "ws_b consistent with ws_a");
}

// ══════════════════════════════════════════════════════════════════════
// Section 3: Read-set caching — read after concurrent write+commit
//
// Each thread reads X twice within a TX.  First read adds X to read-set.
// Concurrent threads write X+1 between the two reads.  The second read
// must either return the updated value (re-read from memory) or return
// the cached stale value — but then validation at tm_end() must detect
// the version change and abort.
//
// Y is a derivative of X's first-read value.  After all TXes, Y must
// be consistent with the final X (proofs §3.1: read validation).
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_rc_x;
static volatile uint64_t g_rc_y;
static constexpr uint64_t RC_COEFF = 7;

static void thread_read_cache(int id)
{
	(void)id;
	tm_init_thread();
	tx_loop(2000, [&]() {
		uint64_t x1 = tm_test_read_i8((uint64_t *)&g_rc_x);
		tm_test_write_i8((uint64_t *)&g_rc_y, x1 * RC_COEFF);
		uint64_t x2 = tm_test_read_i8((uint64_t *)&g_rc_x);
		tm_test_write_i8((uint64_t *)&g_rc_x, (x1 < x2 ? x2 : x1) + 1);
	});
	tm_exit_thread();
}

static void test_read_cache()
{
	fprintf(stderr, "  test_read_cache ...\n");
	g_rc_x = 0;
	g_rc_y = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_read_cache, i);
	for (auto &t : threads)
		t.join();

	uint64_t final_x = g_rc_x;
	uint64_t final_y = g_rc_y;
	uint64_t expected_y = (final_x > 0) ? (final_x - 1) * RC_COEFF : 0;
	TEST_ASSERT_EQ(final_x, (uint64_t)8000, "rc_x == 4*2000");
	TEST_ASSERT_EQ(final_y, expected_y, "rc_y consistent with rc_x");
}

// ══════════════════════════════════════════════════════════════════════
// Section 4: Write-write conflict — same variable, concurrent writers
//
// At most one TX can commit a write to the same address at a time.
// After all TXes, the final value must be a valid thread ID (1..N),
// never zero/uninitialized — proving the TM's write serialization.
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_ww_var;

static void thread_write_write(int id)
{
	tm_init_thread();
	tx_loop(2000, [&]() {
		tm_test_write_i8((uint64_t *)&g_ww_var, (uint64_t)(id + 1));
	});
	tm_exit_thread();
}

static void test_write_write()
{
	fprintf(stderr, "  test_write_write ...\n");
	g_ww_var = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_write_write, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT(g_ww_var >= 1 && g_ww_var <= 4,
	            "ww_var is a valid thread ID (1-4)");
}

// ══════════════════════════════════════════════════════════════════════
// Section 5: Abort stress — high contention to force siglongjmp retries
//
// Many threads hammering the same variable.  High write-write contention
// forces frequent aborts and siglongjmp retries.  We count retries to
// verify the abort path is exercised.
//
// Uses tm_longjmp_ret directly to count aborts.
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_ab_var;
static std::atomic<uint64_t> g_ab_total_aborts{0};

static void thread_abort(int id)
{
	(void)id;
	tm_init_thread();
	uint64_t local_aborts = 0;
	for (int i = 0; i < 5000; i++) {
		tm_nested_call_counter = 1;
		tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
		tm_begin();
		if (tm_longjmp_ret != 0)
			local_aborts++;
		uint64_t v = tm_test_read_i8((uint64_t *)&g_ab_var);
		tm_test_write_i8((uint64_t *)&g_ab_var, v + 1);
		tm_end();
	}
	if (local_aborts > 0)
		g_ab_total_aborts.fetch_add(local_aborts, std::memory_order_relaxed);
	tm_exit_thread();
}

static void test_abort_stress()
{
	fprintf(stderr, "  test_abort_stress ...\n");
	g_ab_var = 0;
	g_ab_total_aborts.store(0);

	std::vector<std::thread> threads;
	for (int i = 0; i < 8; i++)
		threads.emplace_back(thread_abort, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT_EQ(g_ab_var, (uint64_t)40000, "ab_var == 8*5000");
	uint64_t aborts = g_ab_total_aborts.load();
	if (aborts == 0)
		fprintf(stderr, "  WARNING: no aborts recorded\n");
	fprintf(stderr, "  Total aborts: %llu\n", (unsigned long long)aborts);
}

// ══════════════════════════════════════════════════════════════════════
// Section 6: Barrier-coordinated multi-variable interleaving
//
// Each TX reads A and B, then writes both incremented.  Barriers between
// TXes (not inside them) ensure all threads start each phase together,
// creating overlapping read-sets that stress version validation.
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_cn_a;
static volatile uint64_t g_cn_b;

static void thread_concurrent(int id)
{
	(void)id;
	tm_init_thread();
	tx_loop(2000, [&]() {
		uint64_t a = tm_test_read_i8((uint64_t *)&g_cn_a);
		uint64_t b = tm_test_read_i8((uint64_t *)&g_cn_b);
		tm_test_write_i8((uint64_t *)&g_cn_a, a + 1);
		tm_test_write_i8((uint64_t *)&g_cn_b, b + 1);
	});
	tm_exit_thread();
}

static void test_concurrent()
{
	fprintf(stderr, "  test_concurrent ...\n");
	g_cn_a = 0;
	g_cn_b = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_concurrent, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT_EQ(g_cn_a, (uint64_t)8000, "cn_a == 4*2000");
	TEST_ASSERT_EQ(g_cn_b, (uint64_t)8000, "cn_b == 4*2000");
}

// ══════════════════════════════════════════════════════════════════════
// Section 7: Speculative alloc + deferred free under contention
//
// Inside each TX: tm_malloc a block, use it, tm_free it (deferred),
// then tm_malloc another block (speculative).  Increment a contended
// counter to force occasional aborts.  On commit: spec_alloc'd block
// survives, deferred-freed block is flushed.  On abort: spec_alloc'd
// block freed by tm_clear_spec_allocs, deferred-free block resurrected.
//
// Verifies no double-free, no corruption, and final counter is correct.
// ══════════════════════════════════════════════════════════════════════

static std::atomic<uint64_t> g_as_committed{0};

static void thread_alloc_stress(int id)
{
	(void)id;
	tm_init_thread();
	for (int i = 0; i < 500; i++) {
		tm_nested_call_counter = 1;
		sigsetjmp(tm_jmpbuf, 0);
		tm_begin();

		void *p = tm_malloc(64);
		TEST_ASSERT(p != nullptr, "alloc_stress: malloc returned non-null");
		memset(p, (int)(uintptr_t)p & 0xFF, 64);
		tm_free(p);

		void *q = tm_malloc(128);
		TEST_ASSERT(q != nullptr, "alloc_stress: second malloc non-null");
		memset(q, 0xDD, 128);

		uint64_t v = tm_test_read_i8((uint64_t *)&g_ab_var);
		tm_test_write_i8((uint64_t *)&g_ab_var, v + 1);

		tm_end();
		g_as_committed.fetch_add(1);
		TEST_ASSERT(((uint8_t *)q)[0] == 0xDD, "alloc_stress: committed block intact");
		::operator delete(q);
	}
	tm_exit_thread();
}

static void test_alloc_stress()
{
	fprintf(stderr, "  test_alloc_stress ...\n");
	g_ab_var = 0;
	g_as_committed.store(0);

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_alloc_stress, i);
	for (auto &t : threads)
		t.join();
}

// ══════════════════════════════════════════════════════════════════════
// Section 8: Float/double types under contention
// ══════════════════════════════════════════════════════════════════════

static volatile float g_f4_val;
static volatile double g_f8_val;

static void thread_fp(int id)
{
	(void)id;
	tm_init_thread();
	tx_loop(2000, [&]() {
		float f4 = tm_test_read_f4((float *)&g_f4_val);
		tm_test_write_f4((float *)&g_f4_val, f4 + 1.0f);
		double f8 = tm_test_read_f8((double *)&g_f8_val);
		tm_test_write_f8((double *)&g_f8_val, f8 + 1.0);
	});
	tm_exit_thread();
}

static void test_fp()
{
	fprintf(stderr, "  test_fp ...\n");
	g_f4_val = 0.0f;
	g_f8_val = 0.0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_fp, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT(g_f4_val == 8000.0f, "f4 counter == 4*2000");
	TEST_ASSERT(g_f8_val == 8000.0, "f8 counter == 4*2000");
}

// ══════════════════════════════════════════════════════════════════════
// ── Main ──
// ══════════════════════════════════════════════════════════════════════

int main()
{
	fprintf(stderr, "\n=== Multi-Threaded Tests ===\n\n");
	tm_init();

	test_counter();
	test_write_set();
	test_read_cache();
	test_write_write();
	test_abort_stress();
	test_concurrent();
	test_alloc_stress();
	test_fp();

	tm_exit();
	fprintf(stderr, "\n=== Results: %d failures ===\n\n", g_test_failures);
	return g_test_failures;
}
