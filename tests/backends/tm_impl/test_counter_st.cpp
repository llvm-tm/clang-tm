#include "test_helpers.hpp"

static constexpr int ITERATIONS = 100;

int main() {
    printf("Counter ST — single-threaded counter increment\n");
    printf("Iterations: %d\n\n", ITERATIONS);

    tm_init();
    tm_init_thread();
    tm_nested_call_counter++;

    volatile uint64_t counter = 0;

    for (int i = 0; i < ITERATIONS; ++i) {
        tm_transaction([&]() {
            uint64_t v = tm_r8((uint64_t*)&counter);
            tm_w8((uint64_t*)&counter, v + 1);
        });
    }

    tm_nested_call_counter--;
    tm_exit_thread();
    tm_exit();

    return check_result({"counter", counter == ITERATIONS,
                         (int64_t)counter, ITERATIONS});
}
