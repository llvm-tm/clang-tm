/**
 * Write-Set Validation Test
 *
 * Tests that every written address is validated during commit.
 * Without write-set-to-read-set propagation, a transaction can commit
 * after another has written to the same address, causing lost updates.
 *
 * Design: Two shared variables A, B (different 8-byte-aligned addresses,
 * different locks).  Each TX reads A, writes A+1, then writes B = A*COEFF
 * (B is write-set-only — never pre-read).
 *
 * If B is never added to the read-set, validate() ignores it and the
 * final B reflects a stale A snapshot → inconsistent (A, B) pair.
 */

#include "test_helpers.hpp"
#include <thread>
#include <vector>
#include <chrono>

static constexpr int NUM_THREADS = 4;
static constexpr int ITERS_PER_THREAD = 5000;
static constexpr uint64_t COEFF = 10;

struct Data {
    volatile uint64_t a;
    volatile uint64_t b;
};

int main() {
    printf("Write-Set Validation Test\n");
    printf("=========================\n\n");
    printf("Threads:    %d\n", NUM_THREADS);
    printf("Iterations: %d per thread\n", ITERS_PER_THREAD);
    printf("Expected:   a=%d, b=(a-1)*%llu\n\n",
           NUM_THREADS * ITERS_PER_THREAD, (unsigned long long)COEFF);

    tm_init();
    auto data = (Data*)tm_malloc(sizeof(Data));
    data->a = 0;
    data->b = 0;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&data]() {
            tm_init_thread();
            tm_nested_call_counter++;
            for (int j = 0; j < ITERS_PER_THREAD; ++j) {
                tm_transaction([&data]() {
                    uint64_t a_val = tm_r8((uint64_t*)&data->a);
                    tm_w8((uint64_t*)&data->a, a_val + 1);
                    tm_w8((uint64_t*)&data->b, a_val * COEFF);
                });
            }
            tm_nested_call_counter--;
            tm_exit_thread();
        });
    }
    for (auto& th : threads) th.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    tm_exit();

    uint64_t final_a = data->a;
    uint64_t final_b = data->b;
    uint64_t expected_a = (uint64_t)(NUM_THREADS * ITERS_PER_THREAD);
    uint64_t expected_b = (final_a > 0) ? (final_a - 1) * COEFF : 0;

    printf("Results:\n");
    printf("  a = %llu (expected %llu)\n",
           (unsigned long long)final_a, (unsigned long long)expected_a);
    printf("  b = %llu (expected %llu)\n",
           (unsigned long long)final_b, (unsigned long long)expected_b);
    printf("  Time: %llu ms\n\n", (unsigned long long)ms.count());

    int fail = 0;
    if (final_a != expected_a) {
        printf("  FAIL: a=%llu != expected=%llu\n",
               (unsigned long long)final_a, (unsigned long long)expected_a);
        fail = 1;
    }
    if (final_b != expected_b) {
        printf("  FAIL: b=%llu != expected=%llu — lost update on B\n",
               (unsigned long long)final_b, (unsigned long long)expected_b);
        fail = 1;
    }
    printf("\n  Result: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
