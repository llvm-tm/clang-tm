// Regression test for review-02 S04 / TODO.md:40 (NOrec read-only flag semantics).
//
// Scenario under test: a transaction performs READS while tx->read_only is
// still true (i.e. before its first write), then performs a WRITE that clears
// the flag (RO -> RW promotion).  The promotion must not lose the reads:
// every address read during the RO phase must still be recorded in the
// read-set and validated at commit.
//
// Each worker transaction below is written so its first two operations are
// READS (accounts[a], accounts[b]) and its first WRITE comes afterward.  The
// transfer values are taken exclusively from those RO-phase reads.  If an
// implementation dropped/did-not-validate RO-phase reads on promotion, a
// concurrent transfer committing between our reads and our commit would be
// invisible to us, and the final money-conservation invariant would break.
//
// Links against a real backend runtime (NOREC / NORECBF via benchmarks/cpp
// Makefile).  Passes only if money is conserved after many concurrent
// read-first-then-write promotions.
#include "expli_tm_api/tm_api.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

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

namespace
{

constexpr int kAccounts = 16;
constexpr int kThreads = 4;
constexpr int kIters = 50000;

expli::TM<int> g_acct[kAccounts];

int global_txns = 0; // driver-only counter (not TM-annotated: plain monitor)
std::atomic<bool> g_stop{false};

uint32_t lcg(uint32_t &s)
{
	s = s * 1664525u + 1013904223u;
	return s;
}

void worker(int seed)
{
	uint32_t rng = (uint32_t)seed * 2654435761u;
	expli::TM<int>::thread_init();

	for (int i = 0; i < kIters; i++) {
		int a = lcg(rng) % kAccounts;
		int b = lcg(rng) % kAccounts;
		if (a == b)
			b = (b + 1) % kAccounts;

		expli::TM<int>::transaction([&]() {
			// RO phase: read both balances BEFORE any write in this tx.
			// (tx->read_only is still true here.)
			int va = g_acct[a].read();
			int vb = g_acct[b].read();

			// First write: RO -> RW promotion happens on this statement.
			g_acct[a].write(va - 1);
			g_acct[b].write(vb + 1);
		});
	}

	expli::TM<int>::thread_exit();
}

void verify_conservation(const char *label)
{
	int sum = 0;
	for (int i = 0; i < kAccounts; i++)
		sum += g_acct[i].peek();
	CHECK(sum == kAccounts, label);
}

} // namespace

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	expli::TM<int>::init();
	expli::TM<int>::thread_init();

	// Structural check (single-threaded): one tx that reads 10 addresses and
	// writes 1 must commit successfully and the write must be visible.
	{
		expli::TM<int> rw[10];
		for (int i = 0; i < 10; i++)
			rw[i].poke(i);
		expli::TM<int>::transaction([&]() {
			int acc = 0;
			for (int i = 0; i < 10; i++)
				acc += rw[i].read();
			rw[0].write(acc);
		});
		CHECK(rw[0].peek() == 0 + 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9,
		      "read-10-then-write-1 commits and publishes");
	}

	for (int i = 0; i < kAccounts; i++)
		g_acct[i].poke(1);

	std::vector<std::thread> ts;
	for (int t = 0; t < kThreads; t++)
		ts.emplace_back(worker, t + 1);
	for (auto &th : ts)
		th.join();

	verify_conservation("money conserved after RO->RW promotions");
	expli::TM<int>::thread_exit();

	expli::TM<int>::exit();

	if (failures == 0)
		printf("PASS: test_norec_ro2rw (%d checks)\n", tests);
	else
		printf("FAIL: %d/%d checks failed\n", failures, tests);
	return failures == 0 ? 0 : 1;
}