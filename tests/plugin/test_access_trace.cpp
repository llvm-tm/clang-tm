// test_access_trace.cpp — input for the tm-access-trace pass test.
// One transaction with a shared read + shared write (=> 1 tm_read_* + 1
// tm_write_* hook call), and a local that must NOT be traced.
#include <cstdint>
#include <cstdio>

#include "tm_test_common.hpp"

TM int64_t g_x;

TX void bump()
{
	g_x = g_x + 1; // read g_x, then write g_x (both transactional)
	int local = 7; // local, not TM-tracked
	(void)local;
}

MAIN int main()
{
	bump();
	long long v = g_x;
	printf("g_x = %lld (expected 1)\n", v);
	printf("%s\n", v == 1 ? "PASS" : "FAIL");
	fflush(stdout);
	return v == 1 ? 0 : 1;
}
