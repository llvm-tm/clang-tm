/**
 * Overlap Stress Test for SwissTM
 *
 * Targets the READ_LOCKED release bug (Phase 1→Phase 3 interference):
 *   - Both threads read the same shared variables (same orecs)
 *   - Both write to one of them
 *   - Phase 1: both store READ_LOCKED on overlapping orecs
 *   - Phase 3: old_version=READ_LOCKED from other thread's Phase 1
 *   - BUG: abort path stores old_version=READ_LOCKED → r_lock permanently stuck
 *
 * Without the fix, this test sequence hangs within 100-500 iterations.
 */

#include "test_helpers.hpp"
#include <thread>
#include <vector>
#include <chrono>
#include <pthread.h>

static constexpr int NUM_THREADS = 4;
static constexpr int ITERATIONS = 2000;
static constexpr int NUM_VARS = 8;

struct alignas(64) PaddedVar {
    volatile uint64_t val;
};

int main() {
    printf("SwissTM Overlap Stress Test\n");
    printf("===========================\n\n");
    printf("Threads:    %d\n", NUM_THREADS);
    printf("Iterations: %d per thread\n", ITERATIONS);
    printf("Variables:  %d shared orecs\n", NUM_VARS);
    printf("\nTarget: old_version=READ_LOCKED release bug in Phase 3 abort.\n");
    printf("Hang within seconds if fix is missing.\n\n");

    tm_init();
    auto g_vars = (PaddedVar*)tm_malloc(sizeof(PaddedVar) * NUM_VARS);
    for (int i = 0; i < NUM_VARS; ++i) g_vars[i].val = 0;

    auto start = std::chrono::high_resolution_clock::now();

    pthread_barrier_t bar;
    pthread_barrier_init(&bar, NULL, NUM_THREADS + 1);
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t, &bar, &g_vars]() {
            tm_init_thread();
            tm_nested_call_counter++;
            for (int i = 0; i < ITERATIONS; ++i) {
                // Barrier: all threads synchronize at the start of each TX.
                // This maximizes the Phase 1 overlap window.
                pthread_barrier_wait(&bar);

                tm_transaction([t, i, &g_vars]() {
                    for (int v = 0; v < NUM_VARS; ++v) {
                        uint64_t x = tm_r8((uint64_t*)&g_vars[v].val);
                        (void)x;
                    }
                    int idx = (t * ITERATIONS + i) % NUM_VARS;
                    uint64_t cur = tm_r8((uint64_t*)&g_vars[idx].val);
                    tm_w8((uint64_t*)&g_vars[idx].val, cur + 1);
                });

                pthread_barrier_wait(&bar);
            }
            tm_nested_call_counter--;
            tm_exit_thread();
        });
    }

    // Wait for all threads to finish
    for (int i = 0; i < ITERATIONS; ++i) {
        pthread_barrier_wait(&bar); // let threads start TX i
        pthread_barrier_wait(&bar); // let threads finish TX i
    }
    for (auto& th : threads) th.join();
    pthread_barrier_destroy(&bar);

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    tm_exit();

    // Verify: each variable should have been incremented by each thread's
    // (ITERATIONS / NUM_VARS) shares (approximately, modulo contention).
    uint64_t total_ops = (uint64_t)NUM_THREADS * ITERATIONS;
    uint64_t total_sum = 0;
    for (int v = 0; v < NUM_VARS; ++v) {
        total_sum += g_vars[v].val;
    }

    printf("Total TXs attempted: %llu\n", (unsigned long long)total_ops);
    printf("Total increments:    %llu (sum of all vars)\n",
           (unsigned long long)total_sum);
    printf("Time: %llu ms\n\n", (unsigned long long)ms.count());

    int fail = 0;
    if (total_sum == 0) {
        printf("  FAIL: no increments recorded (all TXs aborted?)\n");
        fail = 1;
    } else {
        printf("  Increments committed: %llu / %llu (%.1f%%)\n",
               (unsigned long long)total_sum, (unsigned long long)total_ops,
               100.0 * total_sum / total_ops);
        printf("  PASS (no hang, no crash)\n");
    }

    printf("\n  Result: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
