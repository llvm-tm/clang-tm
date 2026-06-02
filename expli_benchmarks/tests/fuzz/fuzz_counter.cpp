// Fuzz counter — explicit TM API version.
// N threads × K increments on M counters.
// Invariant: final sum = initial sum + total committed.
// Uses raw tm_begin/tm_end with sigsetjmp retry loop to avoid
// the nesting-counter issue in TM<T>::begin()/end().

#include "../../../expli_tm_api/tm_api.hpp"

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

struct Counter {
    expli::TM<uint64_t> val;
};

static Counter *g_counters;
static int g_num_counters;

int main(int argc, char *argv[]) {
    int num_threads = argc > 1 ? atoi(argv[1]) : 4;
    int iters       = argc > 2 ? atoi(argv[2]) : 1000;
    g_num_counters  = argc > 3 ? atoi(argv[3]) : 8;
    unsigned seed   = argc > 4 ? atoi(argv[4]) : 42;

    expli::TM<int>::init();
    g_counters = new Counter[g_num_counters]();

    uint64_t initial = 0;
    for (int i = 0; i < g_num_counters; i++) {
        g_counters[i].val.poke(1000);
        initial += 1000;
    }

    uint64_t *thread_committed = new uint64_t[num_threads]();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([t, iters, seed, &thread_committed]() {
            expli::TM<int>::thread_init();
            unsigned local_seed = seed + t;
            uint64_t local_committed = 0;
            for (int i = 0; i < iters; i++) {
                int idx = rand_r(&local_seed) % g_num_counters;
                int delta = (rand_r(&local_seed) % 10) + 1;
                // Retry loop: tm_nested_call_counter must be 1 before tm_begin
                tm_nested_call_counter++;
                int done = 0;
                while (!done) {
                    tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
                    tm_begin();
                    if (tm_longjmp_ret != 0)
                        continue;
                    uint64_t v = g_counters[idx].val.read();
                    g_counters[idx].val.write(v + delta);
                    tm_end();
                    done = 1;
                }
                tm_nested_call_counter--;
                local_committed += delta;
            }
            thread_committed[t] = local_committed;
            expli::TM<int>::thread_exit();
        });
    }
    for (auto &th : threads)
        th.join();

    uint64_t final_sum = 0;
    for (int i = 0; i < g_num_counters; i++)
        final_sum += g_counters[i].val.peek();

    uint64_t committed = 0;
    for (int t = 0; t < num_threads; t++)
        committed += thread_committed[t];
    uint64_t expected = initial + committed;

    if (final_sum == expected) {
        printf("INVARIANT: counter sum: PASS (%llu == %llu)\n",
               (unsigned long long)final_sum, (unsigned long long)expected);
        delete[] thread_committed;
        delete[] g_counters;
        expli::TM<int>::exit();
        return 0;
    }
    printf("INVARIANT: counter sum: FAIL "
           "(got %llu, expected %llu + %llu, diff=%lld)\n",
           (unsigned long long)final_sum,
           (unsigned long long)initial,
           (unsigned long long)committed,
           (long long)(final_sum - expected));
    delete[] thread_committed;
    delete[] g_counters;
    expli::TM<int>::exit();
    return 1;
}
