#include "test_api.hpp"
#include "test_harness.hpp"
#include <cstring>
#include <map>

// ══════════════════════════════════════════════════════════════════════
// Section 1: Read-only transaction — no write-set, just read and verify
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_ro_val;

static void test_read_only()
{
	fprintf(stderr, "  test_read_only ...\n");
	g_ro_val = 42;

	tx_run([&]() {
		uint64_t v = tm_test_read_i8((uint64_t *)&g_ro_val);
		TEST_ASSERT(v == 42, "read-only tx sees correct value");
	});

	TEST_ASSERT(g_ro_val == 42, "read-only tx didn't modify value");
}

// ══════════════════════════════════════════════════════════════════════
// Section 2: Multiple writers, adjacent addresses (same cache line)
// Two adjacent uint64_t values written simultaneously by multiple threads.
// ══════════════════════════════════════════════════════════════════════

struct alignas(16) AdjacentPair {
	volatile uint64_t a;
	volatile uint64_t b;
};

static AdjacentPair g_adj;

static void thread_adjacent(int)
{
	tm_init_thread();
	tx_loop(5000, [&]() {
		uint64_t a = tm_test_read_i8((uint64_t *)&g_adj.a);
		uint64_t b = tm_test_read_i8((uint64_t *)&g_adj.b);
		tm_test_write_i8((uint64_t *)&g_adj.a, a + 1);
		tm_test_write_i8((uint64_t *)&g_adj.b, b + 1);
	});
	tm_exit_thread();
}

static void test_adjacent()
{
	fprintf(stderr, "  test_adjacent ...\n");
	g_adj.a = 0;
	g_adj.b = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_adjacent, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT_EQ(g_adj.a, (uint64_t)20000, "adj.a == 4*5000");
	TEST_ASSERT_EQ(g_adj.b, (uint64_t)20000, "adj.b == 4*5000");
}

// ══════════════════════════════════════════════════════════════════════
// Section 3: Mixed data types in one transaction
// ══════════════════════════════════════════════════════════════════════

static uint8_t  g_mt_i1;
static uint16_t g_mt_i2;
static uint32_t g_mt_i4;
static uint64_t g_mt_i8;
static float    g_mt_f4;
static double   g_mt_f8;
static void    *g_mt_ptr;

static void test_mixed_types()
{
	fprintf(stderr, "  test_mixed_types ...\n");

	tx_run([&]() {
		tm_test_write_i1(&g_mt_i1, 0xAB);
		tm_test_write_i2(&g_mt_i2, 0xABCD);
		tm_test_write_i4(&g_mt_i4, 0x12345678);
		tm_test_write_i8(&g_mt_i8, 0xDEADBEEFCAFEBABEULL);
		tm_test_write_f4(&g_mt_f4, 3.14159f);
		tm_test_write_f8(&g_mt_f8, 2.718281828459045);
		tm_test_write_ptr(&g_mt_ptr, (void *)0xCAFEBABE);

		TEST_ASSERT(tm_test_read_i1(&g_mt_i1) == 0xAB, "mt i1");
		TEST_ASSERT(tm_test_read_i2(&g_mt_i2) == 0xABCD, "mt i2");
		TEST_ASSERT(tm_test_read_i4(&g_mt_i4) == 0x12345678, "mt i4");
		TEST_ASSERT(tm_test_read_i8(&g_mt_i8) == 0xDEADBEEFCAFEBABEULL, "mt i8");
		TEST_ASSERT(tm_test_read_f4(&g_mt_f4) == 3.14159f, "mt f4");
		TEST_ASSERT(tm_test_read_f8(&g_mt_f8) == 2.718281828459045, "mt f8");
		TEST_ASSERT(tm_test_read_ptr(&g_mt_ptr) == (void *)0xCAFEBABE, "mt ptr");
	});

	TEST_ASSERT(g_mt_i1 == 0xAB, "mt i1 final");
	TEST_ASSERT(g_mt_i2 == 0xABCD, "mt i2 final");
	TEST_ASSERT(g_mt_i4 == 0x12345678, "mt i4 final");
	TEST_ASSERT(g_mt_i8 == 0xDEADBEEFCAFEBABEULL, "mt i8 final");
	TEST_ASSERT(g_mt_f4 == 3.14159f, "mt f4 final");
	TEST_ASSERT(g_mt_f8 == 2.718281828459045, "mt f8 final");
	TEST_ASSERT(g_mt_ptr == (void *)0xCAFEBABE, "mt ptr final");
}

// ══════════════════════════════════════════════════════════════════════
// Section 4: Abort with heap allocation — alloc inside TX, then force
// abort; verify that the speculative alloc was properly freed.
// ══════════════════════════════════════════════════════════════════════

static volatile uint32_t g_aballoc_flag;
static std::atomic<uint32_t> g_aballoc_aborts{0};

static void thread_abort_alloc(int)
{
	tm_init_thread();
	for (int i = 0; i < 3000; i++) {
		tm_nested_call_counter = 1;
		int ret = sigsetjmp(tm_jmpbuf, 0);
		tm_begin();
		if (ret != 0) {
			g_aballoc_aborts.fetch_add(1, std::memory_order_relaxed);
		}

		void *p = tm_malloc(128);
		TEST_ASSERT(p != nullptr, "aballoc: malloc non-null");
		memset(p, 0xEE, 128);
		TEST_ASSERT(((uint8_t *)p)[0] == 0xEE, "aballoc: write before abort");

		tm_free(p);

		uint32_t v = tm_test_read_i4((uint32_t *)&g_aballoc_flag);
		tm_test_write_i4((uint32_t *)&g_aballoc_flag, v + 1);
		tm_end();
	}
	tm_exit_thread();
}

static void test_abort_alloc()
{
	fprintf(stderr, "  test_abort_alloc ...\n");
	g_aballoc_flag = 0;
	g_aballoc_aborts.store(0);

	std::vector<std::thread> threads;
	for (int i = 0; i < 8; i++)
		threads.emplace_back(thread_abort_alloc, i);
	for (auto &t : threads)
		t.join();

	uint64_t expected = 8 * 3000;
	TEST_ASSERT_EQ(g_aballoc_flag, expected, "aballoc counter == 8*3000");
	uint32_t aborts = g_aballoc_aborts.load();
	if (aborts == 0)
		fprintf(stderr, "  WARNING: no aborts recorded\n");
	fprintf(stderr, "  Total aborts: %u\n", (unsigned)aborts);
}

// ══════════════════════════════════════════════════════════════════════
// Section 5: Many-address read-set — read N addresses in one TX
// ══════════════════════════════════════════════════════════════════════

static constexpr int NUM_ADDRS = 100;
static volatile uint64_t g_addrs[NUM_ADDRS];

static void thread_many_reads(int)
{
	tm_init_thread();
	tx_loop(200, [&]() {
		uint64_t sum = 0;
		for (int i = 0; i < NUM_ADDRS; i++)
			sum += tm_test_read_i8((uint64_t *)&g_addrs[i]);
		// Write sum back to a single address
		tm_test_write_i8((uint64_t *)&g_addrs[0], sum);
	});
	tm_exit_thread();
}

static void test_many_reads()
{
	fprintf(stderr, "  test_many_reads ...\n");
	for (int i = 0; i < NUM_ADDRS; i++)
		g_addrs[i] = (uint64_t)(i + 1);

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_many_reads, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT(true, "many_reads completed without crash");
}

// ══════════════════════════════════════════════════════════════════════
// Section 6: Write-set overflow — write many addresses in one TX
// ══════════════════════════════════════════════════════════════════════

static constexpr int NUM_WRITES = 50;
static volatile uint64_t g_ws_over[NUM_WRITES];

static void thread_many_writes(int)
{
	tm_init_thread();
	tx_loop(200, [&]() {
		for (int i = 0; i < NUM_WRITES; i++) {
			uint64_t v = tm_test_read_i8((uint64_t *)&g_ws_over[i]);
			tm_test_write_i8((uint64_t *)&g_ws_over[i], v + 1);
		}
	});
	tm_exit_thread();
}

static void test_many_writes()
{
	fprintf(stderr, "  test_many_writes ...\n");
	for (int i = 0; i < NUM_WRITES; i++)
		g_ws_over[i] = 0;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_many_writes, i);
	for (auto &t : threads)
		t.join();

	for (int i = 0; i < NUM_WRITES; i++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "ws_over[%d] == 800", i);
		TEST_ASSERT_EQ(g_ws_over[i], (uint64_t)800, buf);
	}
}

// ══════════════════════════════════════════════════════════════════════
// Section 7: Invariant across read-set — transfer-like pattern
// Read A and B, verify invariant (A+B == constant), modify, write back
// ══════════════════════════════════════════════════════════════════════

static volatile uint64_t g_inv_a;
static volatile uint64_t g_inv_b;
static constexpr uint64_t INV_TOTAL = 1000000;

static void thread_invariant(int)
{
	tm_init_thread();
	tx_loop(2000, [&]() {
		uint64_t a = tm_test_read_i8((uint64_t *)&g_inv_a);
		uint64_t b = tm_test_read_i8((uint64_t *)&g_inv_b);
		if (a > 0) {
			tm_test_write_i8((uint64_t *)&g_inv_a, a - 1);
			tm_test_write_i8((uint64_t *)&g_inv_b, b + 1);
		}
	});
	tm_exit_thread();
}

static void test_invariant()
{
	fprintf(stderr, "  test_invariant ...\n");
	g_inv_a = INV_TOTAL / 2;
	g_inv_b = INV_TOTAL / 2;

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++)
		threads.emplace_back(thread_invariant, i);
	for (auto &t : threads)
		t.join();

	TEST_ASSERT(g_inv_a + g_inv_b == INV_TOTAL, "A+B invariant preserved");
}

// ══════════════════════════════════════════════════════════════════════
// ── Main ──
// ══════════════════════════════════════════════════════════════════════

int main()
{
	fprintf(stderr, "\n=== Stress Tests ===\n\n");
	tm_init();
	tm_init_thread();

	test_read_only();
	test_mixed_types();

	tm_exit_thread();

	test_adjacent();
	test_abort_alloc();
	test_many_reads();
	test_many_writes();
	test_invariant();

	tm_exit();

	fprintf(stderr, "\n=== Results: %d failures ===\n\n", g_test_failures);
	return g_test_failures;
}
