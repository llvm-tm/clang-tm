#include <cstdio>
#include <thread>

#include "tm_test_common.hpp"

TM int g_counter = 0;

TX void inc() { g_counter = g_counter + 1; }

THREAD void worker()
{
	for (int i = 0; i < 100000; i++)
		inc();
}

MAIN int main()
{
	printf("counter_race: start\n");
	fflush(stdout);
	std::thread t1(worker);
	std::thread t2(worker);
	t1.join();
	t2.join();
	// g_counter is a static TM global: without the B-30 static-global bridge
	// (registerStaticTMSymbols) its accesses bypass the backend's version/lock
	// bookkeeping and the increments are lost. This is the regression test.
	const bool ok = (g_counter == 200000);
	printf("counter_race: g_counter=%d (expected 200000) %s\n",
	       g_counter,
	       ok ? "PASS" : "FAIL");
	fflush(stdout);
	return ok ? 0 : 1;
}
