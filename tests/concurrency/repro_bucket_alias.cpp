/**
 * repro_bucket_alias.cpp — minimal reproducer for version-store aliasing
 * between objects that share a bucket (found by tests/concurrency/
 * drv_diff_ops.cpp).
 *
 * The TM region allocator returns page-aligned blocks, so for an int32
 * array the two halves of any 8-byte word produce the same
 * `(addr >> 3)` bucket index. Every object here is written and read in its
 * OWN transaction with no concurrency at all, and the expected value of
 * each object is tracked in a plain model that only counts committed
 * writes. A backend whose version store is keyed by bucket, whose version
 * nodes do not record which address they belong to, will serve one half of
 * a word for the other; the read-modify-write below then commits that
 * foreign value, so both the transactional read and the committed memory
 * diverge.
 *
 * usage: repro_bucket_alias        exit 0 clean, 2 aliasing detected
 */
#include <csetjmp>
#include <cstdint>
#include <cstdio>

#include "expli_tm_api/tm_api.hpp"

extern __thread int tm_nested_call_counter;
extern __thread sigjmp_buf tm_jmpbuf;

static int failures = 0;
static int *c = nullptr;

// One transaction: read c[idx], optionally RMW +1. `applied` reports whether
// the write committed (retrying forever on abort would hang, so aborts are
// counted and the write retried).
static long long tx_read(int idx, bool increment)
{
	long long v = 0;
	while (true) {
		int r = sigsetjmp(tm_jmpbuf, 0);
		(void)r;
		tm_nested_call_counter = 1;
		tm_set_jmpbuf(&tm_jmpbuf);
		tm_begin();
		v = expli::TM<int>::read_at(&c[idx]);
		if (increment)
			expli::TM<int>::write_at(&c[idx], (int)(v + 1));
		tm_end();
		return v;
	}
}

int main()
{
	constexpr int N = 16;
	constexpr int ROUNDS = 3;
	long long model[N];

	expli::TM<int>::init();
	expli::TM<int>::thread_init();
	c = (int *)expli::TM<int>::malloc(N * sizeof(int));
	for (int i = 0; i < N; i++) {
		c[i] = 1000 + i;
		model[i] = 1000 + i;
	}

	int bad_read = 0, bad_mem = 0;
	int first_bad_read = -1, first_bad_mem = -1;
	long long got_read = 0, want_read = 0, got_mem = 0, want_mem = 0;

	for (int round = 0; round < ROUNDS; round++) {
		for (int i = 0; i < N; i++) {
			tx_read(i, true);
			model[i] += 1; // counted only because tm_end() returned
		}
		for (int i = 0; i < N; i++) {
			long long v = tx_read(i, false);
			if (v != model[i] && first_bad_read < 0) {
				first_bad_read = i;
				got_read = v;
				want_read = model[i];
			}
			if (v != model[i])
				bad_read++;
		}
	}

	for (int i = 0; i < N; i++) {
		if ((long long)c[i] != model[i] && first_bad_mem < 0) {
			first_bad_mem = i;
			got_mem = c[i];
			want_mem = model[i];
		}
		if ((long long)c[i] != model[i])
			bad_mem++;
	}

	printf("transactional reads: %d/%d wrong\n", bad_read, N * ROUNDS);
	if (first_bad_read >= 0)
		printf("  first: c[%d] read %lld, model says %lld\n",
		       first_bad_read,
		       got_read,
		       want_read);
	printf("committed memory:    %d/%d wrong\n", bad_mem, N);
	if (first_bad_mem >= 0)
		printf("  first: c[%d] memory %lld, model says %lld\n",
		       first_bad_mem,
		       got_mem,
		       want_mem);
	printf("memory   =");
	for (int i = 0; i < N; i++)
		printf(" %d", c[i]);
	printf("\nmodel    =");
	for (int i = 0; i < N; i++)
		printf(" %lld", model[i]);
	printf("\n");

	failures = (bad_read || bad_mem) ? 1 : 0;
	printf("\n%s\n", failures ? "BUCKET ALIASING REPRODUCED" : "clean");
	expli::TM<int>::thread_exit();
	expli::TM<int>::exit();
	return failures ? 2 : 0;
}
