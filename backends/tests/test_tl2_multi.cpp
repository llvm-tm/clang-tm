/**
 * Multi-threaded test for simplified TL2_new
 */
#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include "tl2.hpp"

using namespace tl2;

struct SharedData { word_t counter = 0; };
SharedData shared_data;

constexpr int NUM_THREADS = 4;
constexpr int ITERATIONS = 2000;

void thread_func(int tid) {
    init_thread();
    for (int i = 0; i < ITERATIONS; i++) {
        int committed = 0;
        while (!committed) {
            begin();
            word_t val = load(&shared_data.counter);
            val++;
            store(&shared_data.counter, val);
            committed = commit();
        }
    }
    exit_thread();
}

int main() {
    std::cout << "TL2_new Multi-threaded Test\n";
    std::cout << "======================\n";
    init();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(thread_func, i);
    }
    
    for (auto& t : threads) t.join();
    
    std::cout << "Final: " << shared_data.counter << " (expected: " << NUM_THREADS * ITERATIONS << ")\n";
    return shared_data.counter == NUM_THREADS * ITERATIONS ? 0 : 1;
}