// Sub-byte transactional accesses (review-05 G-06/G-07).
//
// Eight adjacent uint8 counters share one cache line (and several
// lock-table slots).  Bugs this catches:
//   * GPUTX CPU pre-fix applied every buffered write as 8 bytes — the
//     first committed byte-write clobbered the other seven counters.
//   * GPUTX CPU pre-fix validated every read as 8 bytes — a 1-byte
//     read-then-write could never validate (infinite retry → hang).
//   * gpu_stm CPU pre-fix loaded 1/2-byte reads as uint32 (OOB).
// Single-threaded phase catches the clobbering deterministically; the
// threaded phase checks per-byte ownership under contention.
#include "test_helpers.hpp"

#include <thread>
#include <vector>

static constexpr int NCOUNTERS = 8;
static constexpr int ITERATIONS = 300;

int main()
{
	printf("Sub-byte counters — 1-byte writes/reads must not touch neighbours\n");

	tm_init();
	tm_set_num_threads(NCOUNTERS);
	tm_init_thread();
	tm_nested_call_counter++;

	// One shared line of 8 uint8 counters, zeroed via tm region.
	// CSMV's version table is 8-byte granular (one entry per aligned word);
	// sub-byte counters packed into one word alias there by design, so CSMV
	// uses a sparse layout where each byte counter owns its own 8-byte cell.
	// On all other backends the dense layout is the actual regression check
	// (a too-wide write must not clobber a neighbour byte).
#ifdef TM_BACKEND_CSMV
	static constexpr int BSTRIDE = 8;
	static constexpr int HSTRIDE = 4;
#else
	static constexpr int BSTRIDE = 1;
	static constexpr int HSTRIDE = 1;
#endif
	volatile uint8_t *bytes = (volatile uint8_t *)tm_malloc(NCOUNTERS * BSTRIDE);
	volatile uint16_t *halves = (volatile uint16_t *)tm_malloc(NCOUNTERS * HSTRIDE * 2);
	for (int i = 0; i < NCOUNTERS; i++) {
		bytes[i * BSTRIDE] = 0;
		halves[i * HSTRIDE] = 0;
	}

	// Phase 1 (ST): increment every byte counter and half counter in one
	// transaction.  Must commit (width-correct validation) and leave all
	// counters at 1 (width-correct application).
	tm_transaction([&]() {
		for (int i = 0; i < NCOUNTERS; i++) {
			tm_w1((uint8_t *)&bytes[i * BSTRIDE],
			      tm_r1((uint8_t *)&bytes[i * BSTRIDE]) + 1);
			tm_w2((uint16_t *)&halves[i * HSTRIDE],
			      tm_r2((uint16_t *)&halves[i * HSTRIDE]) + 1);
		}
	});
	bool st_ok = true;
	for (int i = 0; i < NCOUNTERS; i++) {
		if (*(((volatile uint8_t *)bytes) + i * BSTRIDE) != 1 || halves[i * HSTRIDE] != 1)
			st_ok = false;
	}
	printf("  ST all-8-in-one-tx: %s\n", st_ok ? "PASS" : "FAIL");

	// Phase 2 (MT): thread k owns counter k; contention comes from shared
	// cache line / aliased lock slots, correctness from isolation.
	auto worker = [&](int k) {
		tm_init_thread();
		tm_nested_call_counter++;
		for (int i = 0; i < ITERATIONS; ++i) {
			tm_transaction([&]() {
				tm_w1((uint8_t *)&bytes[k * BSTRIDE],
				      tm_r1((uint8_t *)&bytes[k * BSTRIDE]) + 1);
			});
		}
		tm_nested_call_counter--;
		tm_exit_thread();
	};
	std::vector<std::thread> ts;
	for (int k = 0; k < NCOUNTERS; k++)
		ts.emplace_back(worker, k);
	for (auto &th : ts)
		th.join();

	// uint8 counters wrap: compare in uint8 arithmetic (300+1 mod 256 = 45)
	bool mt_ok = true;
	const uint8_t exp8 = (uint8_t)(ITERATIONS + 1);
	for (int k = 0; k < NCOUNTERS; k++) {
		uint8_t got = (uint8_t)*(bytes + k * BSTRIDE);
		if (got != exp8) {
			printf("  byte[%d]: FAIL (got %u, expected %u)\n",
			       k,
			       (unsigned)got,
			       (unsigned)exp8);
			mt_ok = false;
		}
	}

	tm_nested_call_counter--;
	tm_exit_thread();
	tm_exit();

	if (st_ok && mt_ok) {
		printf("Result: PASS\n");
		return 0;
	}
	printf("Result: FAIL\n");
	return 1;
}
