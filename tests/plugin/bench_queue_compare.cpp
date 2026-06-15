/// Benchmark: compare inline vs queue pipeline for TX execution.
/// Same source file built with both tm-instrument and tm-instrument-queue.
/// Measures throughput (TX/s) for a simple counter increment.

#include <cstdio>
#include <chrono>

extern "C" void (*tm_wait_prev_tx)(void);

static int counter = 0;

__attribute__((noinline, annotate("shared")))
void increment() {
    counter++;
}

int main() {
    // Use a moderate iteration count that completes in < 5s on both pipelines
    const int N = 50000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        increment();
    }
    auto end = std::chrono::high_resolution_clock::now();

    double sec = std::chrono::duration<double>(end - start).count();
    printf("RESULT: %d iterations in %.3f s = %.0f tx/s\n", N, sec, N / sec);
    printf("counter = %d (expected %d)\n", counter, N);

    if (counter != N) {
        printf("FAIL: counter mismatch\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
