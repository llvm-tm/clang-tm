/**
 * repro_width_clobber.cpp — minimal reproducer for sub-8-byte commit
 * width corruption (found by tests/concurrency/drv_diff_ops.cpp).
 *
 * Two adjacent int32 counters and an adjacent byte/word neighbourhood are
 * committed through the explicit API; a correct backend must leave the
 * untouched neighbours alone. A backend that commits at a fixed granularity
 * wider than the access (e.g. write-through of an 8-byte slot for every
 * width) destroys the neighbours.
 *
 * JVSTM (backends/tm_impl/jvstm/jvstm_runtime.cpp) fails the i4 and i1
 * cases: its commit does `memcpy(we.addr, &we.value, sizeof(uint64_t))`
 * for every write regardless of the width that produced it.
 *
 * usage: repro_width_clobber        exit 0 clean, 2 clobber detected
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "expli_tm_api/tm_api.hpp"

static int failures = 0;

#define CHECK(cond, name)                                                                \
	do {                                                                                 \
		if (!(cond)) {                                                                   \
			printf("  FAIL %s\n", (name));                                               \
			failures++;                                                                  \
		} else {                                                                         \
			printf("  ok   %s\n", (name));                                               \
		}                                                                                \
	} while (0)

int main()
{
	expli::TM<int>::init();
	expli::TM<int>::thread_init();

	// One 32-byte TM block: 8 int32 counters, adjacent on purpose.
	int *c = (int *)expli::TM<int>::malloc(8 * sizeof(int));
	for (int i = 0; i < 8; i++)
		c[i] = 1000 + i;
	unsigned char *b = (unsigned char *)expli::TM<int>::malloc(16);
	for (int i = 0; i < 16; i++)
		b[i] = (unsigned char)(10 + i);

	// 1) 4-byte write to c[3] must not disturb c[2] / c[4].
	expli::TM<int>::begin();
	expli::TM<int>::write_at(&c[3], 4321);
	expli::TM<int>::end();
	printf("4-byte write commit:\n");
	CHECK(c[3] == 4321, "written word holds the new value");
	CHECK(c[2] == 1002, "preceding int32 intact");
	CHECK(c[4] == 1004, "following int32 intact");
	printf("      c =");
	for (int i = 0; i < 8; i++)
		printf(" %d", c[i]);
	printf("\n");

	// 2) 1-byte write to b[5] must not disturb b[4] / b[6..7].
	expli::TM<int>::begin();
	expli::TM<unsigned char>::write_at(&b[5], 99);
	expli::TM<int>::end();
	printf("1-byte write commit:\n");
	CHECK(b[5] == 99, "written byte holds the new value");
	CHECK(b[4] == 14, "preceding byte intact");
	CHECK(b[6] == 16, "following byte intact");
	CHECK(b[7] == 17, "byte 7 intact (same 8-byte slot)");
	printf("      b =");
	for (int i = 0; i < 16; i++)
		printf(" %d", b[i]);
	printf("\n");

	// 3) 2-byte write to a uint16 view: same expectation.
	unsigned short *s = (unsigned short *)expli::TM<int>::malloc(8);
	for (int i = 0; i < 8; i++)
		s[i] = (unsigned short)(200 + i);
	expli::TM<int>::begin();
	expli::TM<unsigned short>::write_at(&s[2], 999);
	expli::TM<int>::end();
	printf("2-byte write commit:\n");
	CHECK(s[2] == 999, "written halfword holds the new value");
	CHECK(s[3] == 203, "following halfword intact");
	printf("      s =");
	for (int i = 0; i < 8; i++)
		printf(" %d", s[i]);
	printf("\n");

	printf("\n%s\n", failures ? "WIDTH-CLOBBER REPRODUCED" : "clean");
	expli::TM<int>::thread_exit();
	expli::TM<int>::exit();
	return failures ? 2 : 0;
}
