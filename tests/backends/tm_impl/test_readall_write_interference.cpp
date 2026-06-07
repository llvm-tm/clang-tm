/**
 * SwissTM Read-All + Write Interference Test
 *
 * Targets the "Phase 3 always-validate" hang: long read-all TXs that
 * overlap with concurrent write TXs.  Without the relaxed-ordering +
 * ts > valid_ts + 1 gate, read-all TXs always validate, always detect
 * stale orecs, and always abort → livelock.
 *
 * Design:
 *   - Writer thread: repeatedly increments a single account (hot write)
 *   - Reader threads: repeatedly read ALL accounts (read-all)
 *   - If the hang bug exists, readers never complete (0 committed)
 *
 * Also tests consistency: after all TXs, the bank total must be preserved
 * (every write committed or rolled back correctly).
 */

#include "test_helpers.hpp"
#include <thread>
#include <vector>
#include <chrono>
#include <barrier>
#include <atomic>
#include <atomic>
#include <cstdlib>

static constexpr int NUM_ACCOUNTS = 256;
static constexpr int NUM_WRITERS = 2;
static constexpr int NUM_READERS = 2;
static constexpr int WRITER_ITERS = 1000;
static constexpr int READER_ITERS = 1000;

struct alignas(64) PaddedAccount {
    volatile uint64_t balance;
};

int main() {
    printf("SwissTM Read-All + Write Interference\n");
    printf("======================================\n\n");
    printf("Accounts: %d\n", NUM_ACCOUNTS);
    printf("Writers:  %d x %d iters\n", NUM_WRITERS, WRITER_ITERS);
    printf("Readers:  %d x %d read-all TXs each\n\n", NUM_READERS, READER_ITERS);

    tm_init();
    auto g_accounts = (PaddedAccount*)tm_malloc(sizeof(PaddedAccount) * NUM_ACCOUNTS);
    // Initialize all accounts to 1000
    for (int i = 0; i < NUM_ACCOUNTS; ++i)
        g_accounts[i].balance = 1000;
    uint64_t initial_total = (uint64_t)NUM_ACCOUNTS * 1000;

    std::barrier bar(NUM_WRITERS + NUM_READERS + 1);

    std::atomic<int> writer_committed{0};
    std::atomic<int> reader_committed{0};

    std::vector<std::thread> threads;

    // Writer threads: transfer from account[i] to account[i+1]
    for (int w = 0; w < NUM_WRITERS; ++w) {
        threads.emplace_back([w, &writer_committed, &bar, &g_accounts]() {
            tm_init_thread();
            tm_nested_call_counter++;
            for (int i = 0; i < WRITER_ITERS; ++i) {
                bar.arrive_and_wait();
                tm_transaction([&]() {
                    int from = (w * WRITER_ITERS + i) % NUM_ACCOUNTS;
                    int to   = (from + 1) % NUM_ACCOUNTS;
                    uint64_t bal = tm_r8((uint64_t*)&g_accounts[from].balance);
                    uint64_t add = 1;
                    if (bal >= add) {
                        tm_w8((uint64_t*)&g_accounts[from].balance, bal - add);
                        uint64_t to_bal = tm_r8((uint64_t*)&g_accounts[to].balance);
                        tm_w8((uint64_t*)&g_accounts[to].balance, to_bal + add);
                    }
                });
                bar.arrive_and_wait();
            }
            writer_committed.store(1);
            tm_nested_call_counter--;
            tm_exit_thread();
        });
    }

    // Reader threads: read ALL accounts (read-all TX)
    for (int r = 0; r < NUM_READERS; ++r) {
        threads.emplace_back([r, &reader_committed, &bar, &g_accounts]() {
            tm_init_thread();
            tm_nested_call_counter++;
            for (int i = 0; i < READER_ITERS; ++i) {
                bar.arrive_and_wait();
                tm_transaction([&]() {
                    uint64_t sum = 0;
                    for (int a = 0; a < NUM_ACCOUNTS; ++a) {
                        sum += tm_r8((uint64_t*)&g_accounts[a].balance);
                    }
                    (void)sum;
                });
                bar.arrive_and_wait();
            }
            reader_committed.store(1);
            tm_nested_call_counter--;
            tm_exit_thread();
        });
    }

    // Drive barriers
    int total_iters = WRITER_ITERS; // same count for all
    for (int i = 0; i < total_iters; ++i) {
        bar.arrive_and_wait();
        bar.arrive_and_wait();
    }

    for (auto& th : threads) th.join();

    tm_exit();

    // Compute final total (all backends update memory on commit)
    uint64_t final_total = 0;
    for (int i = 0; i < NUM_ACCOUNTS; ++i)
        final_total += g_accounts[i].balance;

    printf("Initial total: %llu\n", (unsigned long long)initial_total);
    printf("Final total:   %llu\n", (unsigned long long)final_total);
    printf("Writers completed: %d\n", writer_committed.load());
    printf("Readers completed: %d\n\n", reader_committed.load());

    int fail = 0;
    if (final_total != initial_total) {
        printf("  FAIL: money %s (total %llu != %llu)\n",
               final_total > initial_total ? "created" : "lost",
               (unsigned long long)final_total,
               (unsigned long long)initial_total);
        fail = 1;
    }
    if (writer_committed.load() == 0) {
        printf("  FAIL: no writers completed (livelock?)\n");
        fail = 1;
    }
    if (reader_committed.load() == 0) {
        printf("  FAIL: no readers completed (livelock?)\n");
        fail = 1;
    }
    if (!fail) {
        printf("  PASS (money conserved, all threads progressed)\n");
    }
    printf("\n  Result: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
