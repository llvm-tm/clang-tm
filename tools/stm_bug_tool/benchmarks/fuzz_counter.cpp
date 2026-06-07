// Fuzz counter — N threads × K increments on M counters.
// Invariant: final sum = initial sum + total committed.
// Build via fuzz_runner.py, or manually:
//   clang++ -std=c++20 -O0 -pthread -g -I$(PWD) \
//       -DTM_BACKEND_TL2 -Ibackends/TL2 \
//       tools/stm_bug_tool/benchmarks/fuzz_counter.cpp \
//       backends/runtimes/tl2_runtime.cpp

#include "../../../tests/backends/tm_impl/test_helpers.hpp"
#include "../../../backends/tm_impl/common/tm_event_logger.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <thread>
#include <vector>

static uint64_t *g_counters;
static int g_num_counters;
static std::atomic<uint64_t> g_total_committed{0};

int main(int argc, char **argv) {
    int num_threads = argc > 1 ? atoi(argv[1]) : 4;
    int iters = argc > 2 ? atoi(argv[2]) : 1000;
    g_num_counters = argc > 3 ? atoi(argv[3]) : 8;
    unsigned seed = argc > 4 ? atoi(argv[4]) : 42;

    tm_init();
    g_counters = new uint64_t[g_num_counters]();
    for (int i = 0; i < g_num_counters; i++)
        g_counters[i] = 1000;
    uint64_t initial = g_num_counters * 1000ULL;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([t, iters, seed]() {
            tm_init_thread();
            tm_nested_call_counter++;
            unsigned local_seed = seed + t;
            for (int i = 0; i < iters; i++) {
                int idx = rand_r(&local_seed) % g_num_counters;
                int delta = (rand_r(&local_seed) % 10) + 1;
                int committed_delta = 0;
                tm_transaction([&]() {
                    uint64_t v = tm_r8(&g_counters[idx]);
                    tm_w8(&g_counters[idx], v + delta);
                    committed_delta = delta;
                });
                g_total_committed.fetch_add(committed_delta, std::memory_order_relaxed);
            }
            tm_nested_call_counter--;
            TM_EVENT_DUMP(0);
            tm_exit_thread();
        });
    }
    for (auto &th : threads) th.join();

    uint64_t final = 0;
    for (int i = 0; i < g_num_counters; i++)
        final += g_counters[i];
    uint64_t committed = g_total_committed.load();

    if (final == initial + committed) {
        printf("INVARIANT: money conservation: PASS (%llu == %llu + %llu)\n",
               (unsigned long long)final, (unsigned long long)initial,
               (unsigned long long)committed);
    } else {
        printf("INVARIANT: money conservation: FAIL "
               "(got %llu, expected %llu + %llu, diff=%lld)\n",
               (unsigned long long)final, (unsigned long long)initial,
               (unsigned long long)committed,
               (long long)(final - (initial + committed)));
        return 1;
    }
    tm_exit();
    delete[] g_counters;
    return 0;
}
