// mini_diff.cpp — minimal core-invariant test for the T1 approach.
// Two threads, DISJOINT counter ranges, pure ADD deltas via explicit API,
// post-commit shadow flush. If this fails on SGL the invariant/API is broke.
#include "expli_tm_api/tm_api.hpp"
#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

extern __thread int tm_nested_call_counter;
extern __thread sigjmp_buf tm_jmpbuf;

static constexpr int N = 64;
static int *A = nullptr;
static std::atomic<long long> sh[N];
static std::atomic<uint64_t> commits{0}, aborts{0};

int main(int argc, char **argv)
{
	long nops = argc > 1 ? atol(argv[1]) : 5000;
	bool shared = argc > 2 && !strcmp(argv[2], "-shared");
	int span = shared ? N : N / 2;
	expli::TM<int>::init();
	A = (int *)expli::TM<int>::malloc(N * sizeof(int));
	for (int i = 0; i < N; i++)
		A[i] = 1000;

	auto worker = [&](int tid) {
		expli::TM<int>::thread_init();
		std::mt19937 rng(1234u + (unsigned)tid);
		int lo = tid * (N / 2), hi = lo + N / 2;
		(void)hi;
		for (long o = 0; o < nops; o++) {
			std::vector<std::pair<int, long long>> deltas;
			while (true) {
				int r = sigsetjmp(tm_jmpbuf, 0);
				if (r)
					aborts.fetch_add(1);
				tm_nested_call_counter = 1;
				tm_set_jmpbuf(&tm_jmpbuf);
				tm_begin();
				deltas.clear();
				for (int q = 0; q < 4; q++) {
					int i = shared ? (int)(rng() % N) : lo + (int)(rng() % (N / 2));
					long long k = (long long)(rng() % 9) + 1; // positive only
					long long v = expli::TM<int>::read_at(A + i);
					expli::TM<int>::write_at(A + i, (int)(v + k));
					deltas.push_back({i, k});
				}
				tm_end();
				break;
			}
			for (auto &d : deltas)
				sh[d.first].fetch_add(d.second);
			commits.fetch_add(1);
		}
		expli::TM<int>::thread_exit();
	};
	std::vector<std::thread> ts;
	for (int t = 0; t < 2; t++)
		ts.emplace_back(worker, t);
	for (auto &t : ts)
		t.join();

	int viol = 0;
	long long wsum = 0, gsum = 0;
	for (int i = 0; i < N; i++) {
		long long want = 1000 + sh[i].load();
		wsum += want;
		gsum += A[i];
		if ((long long)A[i] != want)
			viol++;
	}
	printf("MINI commits=%llu aborts=%llu viol=%d want=%lld got=%lld diff=%+lld\n",
	       (unsigned long long)commits.load(),
	       (unsigned long long)aborts.load(),
	       viol,
	       wsum,
	       gsum,
	       gsum - wsum);
	expli::TM<int>::exit();
	return viol ? 2 : 0;
}
