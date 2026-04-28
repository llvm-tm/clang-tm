/**
 * Simple SwissTM Test - 2 threads incrementing shared counter
 * No LLVM instrumentation - direct STM API usage
 * 
 * Expected behavior: Final counter should equal NUM_THREADS * ITERATIONS
 */

#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <csetjmp>

extern "C" {
#include "../swissTM/include/stm.h"
}

// Shared data structure
struct SharedData {
    uint64_t counter = 0;
};

// Test parameters
constexpr int NUM_THREADS = 2;
constexpr int ITERATIONS = 10000;

// Global shared data
SharedData shared_data;

// Thread function
void thread_func(int thread_id) {
    // Initialize thread in SwissTM
    wlpdstm_thread_init();
    
    for (int i = 0; i < ITERATIONS; ++i) {
        // Start transaction
        BEGIN_TRANSACTION {
            // Read, increment, write
            uint64_t val = wlpdstm_read_word((Word*)&shared_data.counter);
            val++;
            wlpdstm_write_word((Word*)&shared_data.counter, (Word)val);
        }
        END_TRANSACTION;
    }
    
    wlpdstm_thread_shutdown();
    std::cout << "Thread " << thread_id << " completed\n";
}

int main() {
    std::cout << "SwissTM Simple STM Test\n";
    std::cout << "======================\n";
    std::cout << "Threads:    " << NUM_THREADS << "\n";
    std::cout << "Iterations: " << ITERATIONS << "\n";
    std::cout << "Expected final counter: " << (NUM_THREADS * ITERATIONS) << "\n\n";
    
    // Initialize SwissTM
    wlpdstm_global_init();
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Create and launch threads
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(thread_func, i);
    }
    
    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Cleanup
    wlpdstm_global_shutdown();
    
    std::cout << "\nResults:\n";
    std::cout << "========\n";
    std::cout << "Final counter:  " << shared_data.counter << "\n";
    std::cout << "Expected:       " << (NUM_THREADS * ITERATIONS) << "\n";
    std::cout << "Time elapsed:   " << duration.count() << " ms\n";
    
    if (shared_data.counter == NUM_THREADS * ITERATIONS) {
        std::cout << "\n✓ TEST PASSED: Counter is correct!\n";
        return 0;
    } else {
        std::cout << "\n✗ TEST FAILED: Counter mismatch!\n";
        std::cout << "  Difference: " << ((long long)shared_data.counter - (long long)(NUM_THREADS * ITERATIONS)) << "\n";
        return 1;
    }
}
