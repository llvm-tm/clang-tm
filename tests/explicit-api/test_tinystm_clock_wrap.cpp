// review-02 S03 (ex TODO.md:28 "TinySTM clock wrap-around") — standalone unit
// test that drives the TinySTM version clock to the top of the VERSION field
// (VERSION_MAX) and verifies the reset path in tinystm::increment_clock().
//
// TinySTM's version field is the top (64 - META_BITS) bits of the lock word;
// VERSION_MASK/VERSION_MAX == (2^46 - 1).  increment_clock() detects
// res >= VERSION_MAX, elects ONE thread via reset_locks_thr CAS, zeroes the
// whole lock table (reset_locks), and stores clock = 1.  get_clock() spins
// while clock >= VERSION_MASK, so no reader observes an intermediate state.
//
// This test is header-only (includes tinystm_globals.hpp, which defines the
// TinySTM globals in this TU) and must NOT be linked with TinySTM_runtime.cpp
// (duplicate globals).  Built by tests/explicit-api/Makefile:
//     make test_tinystm_clock_wrap
#include <atomic>
#include <cstdint>
#include <cstdio>

// Same pre-declarations TinySTM_runtime.cpp makes before including the
// TinySTM headers (defined in the runtime .cpp, not needed by this test).
extern "C" {
void tm_serialize_lock();
void tm_serialize_unlock();
int tm_serialize_unlock_all();
}

#ifndef DESIGN_WBCTL
#define DESIGN_WBCTL
#endif
#ifndef TM_BACKEND_TINYSTM
#define TM_BACKEND_TINYSTM
#endif
#include "tinystm_globals.hpp"

static int failures = 0;
static int tests = 0;

#define CHECK(cond, msg)                                                                 \
	do {                                                                                 \
		tests++;                                                                         \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "  FAIL [%s] %s\n", #cond, msg);                             \
			failures++;                                                                  \
		}                                                                                \
	} while (0)

using namespace tinystm;

int main()
{
	// ── Baseline sanity: clock below the wrap threshold increments normally ─
	g_locks_wbctl.reset_versions();
	g_clock.store(5, std::memory_order_release);
	CHECK(increment_clock(1) == 6, "increment below VERSION_MAX returns old+1");
	CHECK(g_clock.load(std::memory_order_acquire) == 6, "g_clock advanced");

	// ── One below wrap: still a plain increment, no reset ──────────────────
	// Plant a non-zero lock state to prove no reset happened.
	g_locks_wbctl.get(0x0).state.store(0xdeadbeef, std::memory_order_release);
	g_clock.store(VERSION_MAX - 2, std::memory_order_release);
	CHECK(increment_clock(1) == VERSION_MAX - 1,
	      "increment at VERSION_MAX-2 returns VERSION_MAX-1");
	CHECK(g_locks_wbctl.get(0x0).state.load(std::memory_order_acquire) == 0xdeadbeef,
	      "no reset below VERSION_MAX (lock state untouched)");

	// ── At the wrap threshold: single reset, clock back to 1, locks zeroed ─
	g_clock.store(VERSION_MAX - 1, std::memory_order_release);
	g_locks_wbctl.get(0x0).state.store(0xdeadbeef, std::memory_order_release);
	word_t res = increment_clock(1);
	CHECK(res == 1, "increment at VERSION_MAX-1 triggers reset -> 1");
	CHECK(g_clock.load(std::memory_order_acquire) == 1, "g_clock reset to 1");
	CHECK(g_locks_wbctl.get(0x0).state.load(std::memory_order_acquire) == 0,
	      "lock table zeroed on wrap");
	CHECK(reset_locks_thr.load(std::memory_order_acquire) == 0,
	      "reset_locks_thr cleared after reset");

	// ── A second wrap from another thread id: CAS election still works ─────
	g_clock.store(VERSION_MAX - 1, std::memory_order_release);
	CHECK(increment_clock(2) == 1, "second wrap (tx_id=2) resets cleanly");
	CHECK(g_clock.load(std::memory_order_acquire) == 1, "g_clock reset to 1 (second)");

	// ── get_clock() sees a clean value after reset ──────────────────────────
	CHECK(get_clock() == 1, "get_clock after wrap returns reset clock");

	if (failures == 0)
		printf("PASS: test_tinystm_clock_wrap (%d checks)\n", tests);
	else
		printf("FAIL: %d/%d checks failed\n", failures, tests);
	return failures == 0 ? 0 : 1;
}
