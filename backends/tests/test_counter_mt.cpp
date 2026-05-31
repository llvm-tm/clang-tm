#include "test_helpers.hpp"
#include <thread>
#include <vector>
#include <chrono>
#include <cstdint>

extern "C" void tm_dbg_set_counter_ptr(const volatile uint64_t *);

static constexpr int NUM_THREADS = 4;
static constexpr int ITERATIONS = 2000;

static volatile uint64_t shared_counter = 0;

int main() {
    printf("Counter MT — multi-threaded counter increment\n");
    printf("Threads:    %d\n", NUM_THREADS);
    printf("Iterations: %d per thread\n", ITERATIONS);
    printf("Expected:   %d\n\n", NUM_THREADS * ITERATIONS);

    tm_init();
    tm_dbg_set_counter_ptr(&shared_counter);

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t]() {
            (void)t;
            tm_init_thread();
            tm_nested_call_counter++;
            for (int i = 0; i < ITERATIONS; ++i) {
                tm_transaction([&]() {
                    uint64_t v = tm_r8((uint64_t*)&shared_counter);
                    tm_w8((uint64_t*)&shared_counter, v + 1);
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

    int64_t expected = (int64_t)NUM_THREADS * ITERATIONS;
    printf("Time: %lld ms\n\n", (long long)ms.count());

    return check_result({"counter", (int64_t)shared_counter == expected,
                         (int64_t)shared_counter, expected});
}
