/**
 * Multi-threaded test for TL2
 */
#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include "tl2.hpp"

using namespace tl2;

volatile uint64_t counter = 0;

constexpr int NUM_THREADS = 2;
constexpr int ITERATIONS = 5000;

void thread_func(int) {
    init_thread();
    for (int i = 0; i < ITERATIONS; i++) {
        int committed = 0;
        while (!committed) {
            begin();
            uint64_t val = tm_read_i8(&counter);
            val++;
            tm_write_i8(&counter, val);
            committed = commit();
            if (!committed) std::this_thread::yield();
        }
    }
    exit_thread();
}

int main() {
    std::cout << "TL2 Multi-threaded Test\n";
    init();

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++)
        threads.emplace_back(thread_func, i);
    for (auto& t : threads) t.join();

    std::cout << "Final: " << counter << " (expected: " << NUM_THREADS * ITERATIONS << ")\n";
    return counter == NUM_THREADS * ITERATIONS ? 0 : 1;
}
