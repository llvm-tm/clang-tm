/**
 * Read-Lock Interference Test (SwissTM-specific)
 *
 * Directly targets the Phase 1/Phase 3 race where two threads hold
 * overlapping read-sets and both commit — triggering the
 * old_version=READ_LOCKED release bug and the token-contention hang.
 *
 * Design: Both threads read the SAME two addresses (same orec or adjacent
 * orecs), then one writes to A and the other writes to B.  Every TX has
 * overlapping read-set entries, guaranteeing Phase 1 interference.
 *
 * Without fix: r_lock gets stuck at READ_LOCKED on abort, causing the
 * next TX to spin forever on read_impl.
 */

#include "test_helpers.hpp"
#include <thread>
#include <chrono>
#include <barrier>

static constexpr int NUM_THREADS = 2;
static constexpr int ITERATIONS = 5000;

struct alignas(64) PaddedVar {
    volatile uint64_t val;
};

static PaddedVar g_a{0};
static PaddedVar g_b{0};

int main() {
    printf("Read-Lock Interference Test\n");
    printf("===========================\n\n");
    printf("Threads:    %d\n", NUM_THREADS);
    printf("Iterations: %d per thread\n\n", ITERATIONS);

    tm_init();

    auto start = std::chrono::high_resolution_clock::now();

    std::barrier bar(NUM_THREADS + 1);
    std::thread t0([&]() {
        tm_init_thread();
        tm_nested_call_counter++;
        for (int i = 0; i < ITERATIONS; ++i) {
            bar.arrive_and_wait();
            tm_transaction([&]() {
                // Read both — overlap with thread 1's read set
                uint64_t av = tm_r8((uint64_t*)&g_a.val);
                uint64_t bv = tm_r8((uint64_t*)&g_b.val);
                (void)av; (void)bv;
                tm_w8((uint64_t*)&g_a.val, g_a.val + 1);
            });
            bar.arrive_and_wait();
        }
        tm_nested_call_counter--;
        tm_exit_thread();
    });

    std::thread t1([&]() {
        tm_init_thread();
        tm_nested_call_counter++;
        for (int i = 0; i < ITERATIONS; ++i) {
            bar.arrive_and_wait();
            tm_transaction([&]() {
                // Read both — overlap with thread 0's read set
                uint64_t av = tm_r8((uint64_t*)&g_a.val);
                uint64_t bv = tm_r8((uint64_t*)&g_b.val);
                (void)av; (void)bv;
                tm_w8((uint64_t*)&g_b.val, g_b.val + 1);
            });
            bar.arrive_and_wait();
        }
        tm_exit_thread();
    });

    // Drive the main barrier
    for (int i = 0; i < ITERATIONS; ++i) {
        bar.arrive_and_wait();
        bar.arrive_and_wait();
    }

    t0.join();
    t1.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    tm_exit();

    uint64_t final_a = g_a.val;
    uint64_t final_b = g_b.val;
    uint64_t expected_a = ITERATIONS;
    uint64_t expected_b = ITERATIONS;

    printf("A = %llu (expected %llu)\n",
           (unsigned long long)final_a, (unsigned long long)expected_a);
    printf("B = %llu (expected %llu)\n",
           (unsigned long long)final_b, (unsigned long long)expected_b);
    printf("Total TXs attempted: %d\n", 2 * ITERATIONS);
    printf("Total increments:    %llu\n",
           (unsigned long long)(final_a + final_b));
    printf("Time: %llu ms\n\n", (unsigned long long)ms.count());

    int fail = 0;
    if (final_a + final_b == 0) {
        printf("  FAIL: no increments — hang or livelock\n");
        fail = 1;
    } else if (ms.count() > 30000) {
        printf("  FAIL: completed but abnormally slow (%llu ms)\n",
               (unsigned long long)ms.count());
        fail = 1;
    } else {
        printf("  PASS (%llu ms, no hang)\n", (unsigned long long)ms.count());
    }
    printf("\n  Result: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
