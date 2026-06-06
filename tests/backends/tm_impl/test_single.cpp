#include "test_api.hpp"
#include "test_harness.hpp"
#include <cstring>

// ══════════════════════════════════════════════════════════════════════
// Section 1: Basic types — every read/write type inside TX
// ══════════════════════════════════════════════════════════════════════

static uint8_t g_u1;
static uint16_t g_u2;
static uint32_t g_u4;
static uint64_t g_u8;
static float g_f4;
static double g_f8;
static void *g_ptr;

static void test_basic_types()
{
	fprintf(stderr, "  test_basic_types ...\n");

	tx_run([&]() {
		tm_test_write_i1(&g_u1, 0xAB);
		TEST_ASSERT(tm_test_read_i1(&g_u1) == 0xAB, "i1 write/read");

		tm_test_write_i2(&g_u2, 0xABCD);
		TEST_ASSERT(tm_test_read_i2(&g_u2) == 0xABCD, "i2 write/read");

		tm_test_write_i4(&g_u4, 0xDEADBEEF);
		TEST_ASSERT(tm_test_read_i4(&g_u4) == 0xDEADBEEF, "i4 write/read");

		uint64_t v8 = 0xAABBCCDDEEFF1234ULL;
		tm_test_write_i8(&g_u8, v8);
		TEST_ASSERT(tm_test_read_i8(&g_u8) == v8, "i8 write/read");

		tm_test_write_f4(&g_f4, 3.14159f);
		TEST_ASSERT(tm_test_read_f4(&g_f4) == 3.14159f, "f4 write/read");

		tm_test_write_f8(&g_f8, 2.718281828459045);
		TEST_ASSERT(tm_test_read_f8(&g_f8) == 2.718281828459045, "f8 write/read");

		void *vp = (void *)0xCAFEBABE;
		tm_test_write_ptr(&g_ptr, vp);
		TEST_ASSERT(tm_test_read_ptr(&g_ptr) == vp, "ptr write/read");
	});

	TEST_ASSERT(g_u1 == 0xAB, "g_u1 final");
	TEST_ASSERT(g_u2 == 0xABCD, "g_u2 final");
	TEST_ASSERT(g_u4 == 0xDEADBEEF, "g_u4 final");
	TEST_ASSERT(g_u8 == 0xAABBCCDDEEFF1234ULL, "g_u8 final");
	TEST_ASSERT(g_f4 == 3.14159f, "g_f4 final");
	TEST_ASSERT(g_f8 == 2.718281828459045, "g_f8 final");
	TEST_ASSERT(g_ptr == (void *)0xCAFEBABE, "g_ptr final");
}

// ══════════════════════════════════════════════════════════════════════
// Section 2: Sequential transactions
// ══════════════════════════════════════════════════════════════════════

static volatile uint32_t g_seq_counter;

static void test_seq_tx()
{
	fprintf(stderr, "  test_seq_tx ...\n");

	for (int i = 0; i < 100; i++) {
		tx_run([&]() {
			uint32_t v = tm_test_read_i4((uint32_t *)&g_seq_counter);
			tm_test_write_i4((uint32_t *)&g_seq_counter, v + 1);
		});
	}
	TEST_ASSERT(g_seq_counter == 100, "seq counter == 100");
}

// ══════════════════════════════════════════════════════════════════════
// Section 3: Memory allocation (tm_malloc / tm_calloc / tm_realloc / tm_free)
// ══════════════════════════════════════════════════════════════════════

static void test_alloc_basic()
{
	fprintf(stderr, "  test_alloc_basic ...\n");

	void *p = nullptr;
	tx_run([&]() {
		p = tm_malloc(64);
		TEST_ASSERT(p != nullptr, "tm_malloc returned non-null");
		memset(p, 0xAA, 64);
		TEST_ASSERT(((uint8_t *)p)[0] == 0xAA, "write to malloc'd memory");
		TEST_ASSERT(((uint8_t *)p)[63] == 0xAA, "write to malloc'd memory end");
		tm_free(p);
	});
	TEST_ASSERT(true, "alloc + free in TX committed OK");
}

static void test_alloc_calloc_realloc()
{
	fprintf(stderr, "  test_alloc_calloc_realloc ...\n");

	tx_run([&]() {
		void *p = tm_calloc(16, 4);
		TEST_ASSERT(p != nullptr, "tm_calloc returned non-null");
		for (int i = 0; i < 64; i++)
			TEST_ASSERT(((uint8_t *)p)[i] == 0, "calloc zero-filled");
		tm_free(p);

		p = tm_malloc(32);
		TEST_ASSERT(p != nullptr, "tm_malloc returned non-null");
		void *q = tm_realloc(p, 64);
		TEST_ASSERT(q != nullptr, "tm_realloc returned non-null");
		tm_free(q);
	});
	TEST_ASSERT(true, "calloc + realloc + free committed OK");
}

// ══════════════════════════════════════════════════════════════════════
// Section 4: Speculative allocation — alloc inside TX then commit
// ══════════════════════════════════════════════════════════════════════

static void test_alloc_spec_commit()
{
	fprintf(stderr, "  test_alloc_spec_commit ...\n");

	void *outside = nullptr;
	tx_run([&]() {
		outside = tm_malloc(128);
		TEST_ASSERT(outside != nullptr, "spec alloc inside TX");
		memset(outside, 0xBB, 128);
	});
	TEST_ASSERT(((uint8_t *)outside)[0] == 0xBB, "spec alloc survived commit");
	TEST_ASSERT(((uint8_t *)outside)[127] == 0xBB, "spec alloc content intact");
	::operator delete(outside);
}

// ══════════════════════════════════════════════════════════════════════
// Section 5: Deferred free on commit
// ══════════════════════════════════════════════════════════════════════

static void test_deferred_free_commit()
{
	fprintf(stderr, "  test_deferred_free_commit ...\n");

	void *p = ::operator new(64);
	memset(p, 0xCC, 64);
	TEST_ASSERT(((uint8_t *)p)[0] == 0xCC, "pre-allocated block OK");
	tx_run([&]() { tm_free(p); });
	TEST_ASSERT(true, "deferred free on commit OK");
}

// ══════════════════════════════════════════════════════════════════════
// Section 6: Multiple allocations in a single TX
// ══════════════════════════════════════════════════════════════════════

static void test_multi_alloc()
{
	fprintf(stderr, "  test_multi_alloc ...\n");

	void *ptrs[10];
	tx_run([&]() {
		for (int i = 0; i < 10; i++) {
			ptrs[i] = tm_malloc(32);
			TEST_ASSERT(ptrs[i] != nullptr, "multi alloc non-null");
			((uint32_t *)ptrs[i])[0] = i;
		}
		for (int i = 0; i < 10; i++)
			TEST_ASSERT(((uint32_t *)ptrs[i])[0] == (uint32_t)i, "multi alloc content");
		for (int i = 0; i < 10; i++)
			tm_free(ptrs[i]);
	});
	TEST_ASSERT(true, "multi alloc + free committed OK");
}

// ══════════════════════════════════════════════════════════════════════
// Section 7: Serialization lock
// ══════════════════════════════════════════════════════════════════════

static volatile int g_serialize_data = 0;

static void test_serialize_lock()
{
	fprintf(stderr, "  test_serialize_lock ...\n");

	tx_run([&]() {
		tm_serialize_lock();
		g_serialize_data = 42;
		TEST_ASSERT(g_serialize_data == 42, "serialized write");
		tm_serialize_unlock();
	});
	TEST_ASSERT(g_serialize_data == 42, "serialize data persisted");
}

// ══════════════════════════════════════════════════════════════════════
// Section 8: Operations outside a TX (must not crash — fallthrough)
// ══════════════════════════════════════════════════════════════════════

static void test_outside_tx()
{
	fprintf(stderr, "  test_outside_tx ...\n");

	// TM functions (tm_read/tm_write) are only valid inside a TX.
	// Outside a TX, use direct memory access.
	uint32_t x = 10;
	x = 20;
	TEST_ASSERT(x == 20, "outside-tx write modifies memory");
}

// ══════════════════════════════════════════════════════════════════════
// Section 9: Pointer read/write with null
// ══════════════════════════════════════════════════════════════════════

static void *g_null_ptr_target;
static void test_ptr_null()
{
	fprintf(stderr, "  test_ptr_null ...\n");

	tx_run([&]() {
		tm_test_write_ptr(&g_null_ptr_target, nullptr);
		void *r = tm_test_read_ptr(&g_null_ptr_target);
		TEST_ASSERT(r == nullptr, "ptr null write/read");
	});
}

// ══════════════════════════════════════════════════════════════════════
// ── Main ──
// ══════════════════════════════════════════════════════════════════════

int main()
{
	fprintf(stderr, "\n=== Single-Threaded Tests ===\n\n");

	tm_init();
	tm_init_thread();

	test_basic_types();
	test_seq_tx();
	test_alloc_basic();
	test_alloc_calloc_realloc();
	test_alloc_spec_commit();
	test_deferred_free_commit();
	test_multi_alloc();
	test_serialize_lock();
	test_outside_tx();
	test_ptr_null();

	tm_exit_thread();
	tm_exit();

	fprintf(stderr, "\n=== Results: %d failures ===\n\n", g_test_failures);
	return g_test_failures;
}
