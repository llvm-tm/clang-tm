// Fuzz bank — explicit TM API version.
// Random transfers between N accounts in parallel.
// Invariant: sum of all accounts is preserved after all TXs.
//
// Build:
//   make -C expli_benchmarks BACKEND=TINYSTM run-fuzz-bank
// or manually:
//   clang++ -std=c++20 -O0 -pthread -g -I.. \
//       -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL -I../backends -I../backends/TinySTM \
//       expli_benchmarks/fuzz/fuzz_bank.cpp \
//       backends/runtimes/TinySTM_runtime.cpp

#include "../../expli_instr/cpp/include/tm_api.hpp"

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

struct Account {
    expli::TM<uint32_t> balance;
};

static Account *g_accounts;
static int g_num_accounts;

int main(int argc, char *argv[]) {
    int num_threads  = argc > 1 ? atoi(argv[1]) : 4;
    int iters        = argc > 2 ? atoi(argv[2]) : 1000;
    g_num_accounts   = argc > 3 ? atoi(argv[3]) : 64;
    unsigned seed    = argc > 4 ? atoi(argv[4]) : 42;

    expli::TM<int>::init();
    g_accounts = new Account[g_num_accounts]();

    uint64_t initial = 0;
    for (int i = 0; i < g_num_accounts; i++) {
        g_accounts[i].balance.poke(10000);
        initial += 10000;
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([t, iters, seed]() {
            expli::TM<int>::thread_init();
            unsigned local_seed = seed + t;
            for (int i = 0; i < iters; i++) {
                int src = rand_r(&local_seed) % g_num_accounts;
                int dst = rand_r(&local_seed) % g_num_accounts;
                int amount = (rand_r(&local_seed) % 100) + 1;
                if (src == dst)
                    continue;
                tm_nested_call_counter++;
                int done = 0;
                while (!done) {
                    tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
                    tm_begin();
                    if (tm_longjmp_ret != 0)
                        continue;
                    uint32_t sb = g_accounts[src].balance.read();
                    if (sb >= (uint32_t)amount) {
                        uint32_t db = g_accounts[dst].balance.read();
                        g_accounts[src].balance.write(sb - amount);
                        g_accounts[dst].balance.write(db + amount);
                    }
                    tm_end();
                    done = 1;
                }
                tm_nested_call_counter--;
            }
            expli::TM<int>::thread_exit();
        });
    }
    for (auto &th : threads)
        th.join();

    uint64_t final_sum = 0;
    for (int i = 0; i < g_num_accounts; i++)
        final_sum += g_accounts[i].balance.peek();

    if (final_sum == initial) {
        printf("INVARIANT: money conservation: PASS (%llu == %llu)\n",
               (unsigned long long)final_sum, (unsigned long long)initial);
        delete[] g_accounts;
        expli::TM<int>::exit();
        return 0;
    }
    printf("INVARIANT: money conservation: FAIL "
           "(got %llu, expected %llu, diff=%lld)\n",
           (unsigned long long)final_sum, (unsigned long long)initial,
           (long long)(final_sum - initial));
    delete[] g_accounts;
    expli::TM<int>::exit();
    return 1;
}
