#pragma once
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static int g_test_failures = 0;

#define TEST_ASSERT(cond, msg)                                                             \
	do {                                                                                   \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);              \
			g_test_failures++;                                                             \
		}                                                                                  \
	} while (0)

#define TEST_ASSERT_EQ(a, b, msg)                                                          \
	do {                                                                                   \
		if ((a) != (b)) {                                                                  \
			fprintf(stderr,                                                                \
			        "  FAIL [%s:%d] %s: expected %llu, got %llu\n",                       \
			        __FILE__,                                                              \
			        __LINE__,                                                              \
			        msg,                                                                   \
			        (unsigned long long)(b),                                               \
			        (unsigned long long)(a));                                              \
			g_test_failures++;                                                             \
		}                                                                                  \
	} while (0)

struct Barrier {
	std::atomic<int> count_{0};
	int n_;
	Barrier(int n)
	    : n_(n)
	{
	}
	void wait()
	{
		int prev = count_.fetch_add(1, std::memory_order_acq_rel);
		while (count_.load(std::memory_order_acquire) < n_) {}
		if (prev + 1 == n_) reset();
	}
	void reset() { count_.store(0, std::memory_order_release); }
};

struct Timer {
	std::chrono::high_resolution_clock::time_point start_;
	Timer() { reset(); }
	void reset() { start_ = std::chrono::high_resolution_clock::now(); }
	int64_t elapsed_ms() const
	{
		auto end = std::chrono::high_resolution_clock::now();
		return std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();
	}
};

// TX helper for backends that use tm_nested_call_counter (TinySTM, TL2, SwissTM, SGL).
// Sets tm_nested_call_counter = 1 so tm_begin()/tm_end() execute the outer-TX path.
// On siglongjmp retry, tm_nested_call_counter is still 1 (TLS not restored by longjmp),
// so tm_begin() correctly starts a new TX on each retry.
template <typename F> inline void tx_run(F &&f)
{
	tm_nested_call_counter = 1;
	sigsetjmp(tm_jmpbuf, 0);
	tm_begin();
	f();
	tm_end();
}

// Looped TX variant for multi-threaded tests.
// Each iteration: set counter, sigsetjmp, tm_begin, ops, tm_end.
// If abort occurs, siglongjmp returns to sigsetjmp with non-zero retval;
// tm_begin() starts a fresh TX (counter is still 1), then ops run again.
// WARNING: not compatible with NOrec (which gates tm_begin on tm_longjmp_ret).
template <typename F> inline void tx_loop(int iters, F &&f)
{
	for (int i = 0; i < iters; i++) {
		tm_nested_call_counter = 1;
		sigsetjmp(tm_jmpbuf, 0);
		tm_begin();
		f();
		tm_end();
	}
}
