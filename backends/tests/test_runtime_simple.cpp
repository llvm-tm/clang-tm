/**
 * Simple Runtime Test - uses tm_* stubs
 * This test can work with any TM implementation by linking different runtime files
 */

#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include <atomic>
#include <chrono>

// Runtime stubs - implementation provided by linking with appropriate runtime file
extern "C" {
    void tm_init();
    void tm_exit();
    void tm_init_thread();
    void tm_exit_thread();
    int tm_begin();  // Returns 1 on success, 0 on abort
    int tm_end();  // Returns 1 on success,0 on failure
    uint32_t tm_read_i4(volatile uint32_t* addr);
    void tm_write_i4(volatile uint32_t* addr, uint32_t val);
}

struct SharedData {
    volatile uint32_t counter;
    
    SharedData() : counter(0) {}
};

constexpr int NUM_THREADS = 4;
constexpr int ITERATIONS = 2000;

SharedData shared_data;

void thread_func(int thread_id) {
    tm_init_thread();
    
    for (int i = 0; i < ITERATIONS; ++i) {
        int committed = 0;
        while (!committed) {
            if (!tm_begin()) {
                continue;
            }
            uint32_t val = tm_read_i4(&shared_data.counter);
            val++;
            tm_write_i4(&shared_data.counter, val);
            committed = tm_end();
        }
    }
    
    tm_exit_thread();
}

int main() {
    std::cout << "Runtime Simple Test\n";
    std::cout << "====================\n";
    std::cout << "Threads:    " << NUM_THREADS << "\n";
    std::cout << "Iterations: " << ITERATIONS << "\n";
    std::cout << "Expected final counter: " << (NUM_THREADS * ITERATIONS) << "\n\n";
    
    tm_init();
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(thread_func, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    tm_exit();
    
    std::cout << "\nResults:\n";
    std::cout << "========\n";
    std::cout << "Final counter:  " << shared_data.counter << "\n";
    std::cout << "Expected:       " << (NUM_THREADS * ITERATIONS) << "\n";
    std::cout << "Time elapsed:   " << duration.count() << " ms\n";
    
    if (shared_data.counter == NUM_THREADS * ITERATIONS) {
        std::cout << "\nTEST PASSED\n";
        return 0;
    } else {
        std::cout << "\nTEST FAILED (counter: " << shared_data.counter 
                  << ", expected: " << (NUM_THREADS * ITERATIONS) << ")\n";
        return 1;
    }
}
