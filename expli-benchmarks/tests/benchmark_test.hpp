#pragma once
// Shared test harness for expli-benchmark self-tests (--test mode).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

static int g_test_failures = 0;
static int g_test_count = 0;

#define TEST_ASSERT(cond, msg)                                                             \
	do {                                                                                   \
		g_test_count++;                                                                    \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);              \
			g_test_failures++;                                                             \
		}                                                                                  \
	} while (0)

#define TEST_EQ(a, b, msg)                                                                 \
	do {                                                                                   \
		g_test_count++;                                                                    \
		if ((a) != (b)) {                                                                  \
			fprintf(stderr,                                                                \
			        "  FAIL [%s:%d] %s: expected %lld, got %lld\n",                       \
			        __FILE__, __LINE__, msg,                                               \
			        (long long)(b), (long long)(a));                                       \
			g_test_failures++;                                                             \
		}                                                                                  \
	} while (0)

#define TEST_NEAR(a, b, eps, msg)                                                          \
	do {                                                                                   \
		g_test_count++;                                                                    \
		double diff = (double)(a) - (double)(b);                                           \
		if (diff < 0) diff = -diff;                                                        \
		if (diff > (eps)) {                                                                \
			fprintf(stderr,                                                                \
			        "  FAIL [%s:%d] %s: expected %.10f, got %.10f (diff %.10f > %.10f)\n", \
			        __FILE__, __LINE__, msg,                                               \
			        (double)(b), (double)(a), diff, (double)(eps));                        \
			g_test_failures++;                                                             \
		}                                                                                  \
	} while (0)

static int test_result() {
	if (g_test_failures == 0) {
		printf("  PASS (%d assertions)\n", g_test_count);
	} else {
		printf("  FAIL (%d / %d assertions failed)\n", g_test_failures, g_test_count);
	}
	int ret = g_test_failures;
	g_test_failures = 0;
	g_test_count = 0;
	return ret;
}

// ── RNG determinism test ────────────────────────────────
// Verifies that a PRNG with the same seed produces the same sequence.
template<typename RNG>
static void test_rng_determinism() {
	RNG a(42), b(42);
	for (int i = 0; i < 1000; i++) {
		TEST_EQ(a(), b(), "RNG determinism");
	}
}

// ── CLI flag parsing test ───────────────────────────────
// Reusable test that verifies a benchmark's flag parser.
// Usage: call with argc/argv replicating flag combos, then
// check the global config variables match expectations.
