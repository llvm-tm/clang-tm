// Minimal repro for review-04/BUGS.md B-30:
// TL2 and SwissTM plugin-mode read-modify-write race in the simplest
// possible case (plain global int, two threads, two-phase OCC backends).
//
// Build via the plugin pipeline (any backend whose runtime supports
// OCC + read-set validation). Expected: g_counter == N*T. Observed on
// 2026-09-21: TL2 ~9200-9950, SwissTM ~8700-9300; NOREC and
// SingleGlobalLock both pass (they are not OCC).
//
// See review-04/ROADMAP.md R-28 / R-28b for the investigation notes.

#include <atomic>
#include <cstdio>
#include <thread>

#include "tm_test_common.hpp"

TM int g_counter = 0;

TX void inc() { g_counter = g_counter + 1; }

THREAD void worker(int n)
{
	for (int i = 0; i < n; i++)
		inc();
}

MAIN int main()
{
	constexpr int N = 5000;
	std::thread t1(worker, N);
	std::thread t2(worker, N);
	t1.join();
	t2.join();
	printf("g_counter=%d (expected %d)\n", g_counter, 2 * N);
	return g_counter == 2 * N ? 0 : 1;
}
