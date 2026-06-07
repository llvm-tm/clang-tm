// Fuzz bank — random transfers between N accounts in parallel.
// Invariant: sum of all accounts is preserved after all TXs.
// Build via fuzz_runner.py, or manually:
//   clang++ -std=c++20 -O0 -pthread -g -I$(PWD) \
//       -DTM_EVENT_LOG -DTM_BACKEND_TL2 -Ibackends/TL2 \
//       tools/stm_bug_tool/benchmarks/fuzz_bank.cpp \
//       backends/runtimes/tl2_runtime.cpp

#include "../../../tests/backends/tm_impl/test_helpers.hpp"
#include "../../../backends/tm_impl/common/tm_event_logger.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static uint32_t *g_accounts;
static int g_num_accounts;
static std::atomic<uint64_t> g_total_transferred{0};

int main(int argc, char **argv) {
    int num_threads = argc > 1 ? atoi(argv[1]) : 4;
    int iters = argc > 2 ? atoi(argv[2]) : 1000;
    g_num_accounts = argc > 3 ? atoi(argv[3]) : 64;
    unsigned seed = argc > 4 ? atoi(argv[4]) : 42;

    tm_init();
    g_accounts = new uint32_t[g_num_accounts]();
    for (int i = 0; i < g_num_accounts; i++)
        g_accounts[i] = 10000;
    uint64_t initial = g_num_accounts * 10000ULL;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([t, iters, seed]() {
            tm_init_thread();
            tm_nested_call_counter++;
            unsigned local_seed = seed + t;
            for (int i = 0; i < iters; i++) {
                int src = rand_r(&local_seed) % g_num_accounts;
                int dst = rand_r(&local_seed) % g_num_accounts;
                int amount = (rand_r(&local_seed) % 100) + 1;
                if (src == dst) continue;
                int committed = 0;
                tm_transaction([&]() {
                    uint32_t sb = tm_r4(&g_accounts[src]);
                    if (sb < (uint32_t)amount) return;
                    uint32_t db = tm_r4(&g_accounts[dst]);
                    tm_w4(&g_accounts[src], sb - amount);
                    tm_w4(&g_accounts[dst], db + amount);
                    committed = amount;
                });
                g_total_transferred.fetch_add(committed, std::memory_order_relaxed);
            }
            tm_nested_call_counter--;
            TM_EVENT_DUMP(0);
            tm_exit_thread();
        });
    }
    for (auto &th : threads) th.join();

    uint64_t final = 0;
    for (int i = 0; i < g_num_accounts; i++)
        final += g_accounts[i];

    if (final == initial) {
        printf("INVARIANT: money conservation: PASS (%llu == %llu)\n",
               (unsigned long long)final, (unsigned long long)initial);
    } else {
        printf("INVARIANT: money conservation: FAIL "
               "(got %llu, expected %llu, diff=%lld)\n",
               (unsigned long long)final, (unsigned long long)initial,
               (long long)(final - initial));
        return 1;
    }

    TM_EVENT_DUMP(0);
    tm_exit();
    delete[] g_accounts;
    return 0;
}
