/**
 * Debug Test - tracks begin/end/abort counts using local counting
 */

#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <cassert>

extern "C" {
    void tm_init();
    void tm_exit();
    void tm_init_thread();
    void tm_exit_thread();
    int tm_begin();
    int tm_end();
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
std::atomic<int> thread_end_success{0};
std::atomic<int> thread_end_fail{0};

void thread_func(int thread_id) {
    tm_init_thread();
    
    int local_success = 0;
    int local_fail = 0;
    
    for (int i = 0; i < ITERATIONS; ++i) {
        int committed = 0;
        while (!committed) {
            int begin_result = tm_begin();
            if (!begin_result) {
                local_fail++;
                continue;
            }
            uint32_t val = tm_read_i4(&shared_data.counter);
            val++;
            tm_write_i4(&shared_data.counter, val);
            committed = tm_end();
            
            if (committed) {
                local_success++;
            } else {
                local_fail++;
            }
        }
    }
    
    std::cout << "Thread " << thread_id << ": success=" << local_success 
              << ", fail=" << local_fail << "\n";
    
    thread_end_success += local_success;
    thread_end_fail += local_fail;
    
    tm_exit_thread();
}

int main() {
    std::cout << "Debug Test\n";
    std::cout << "==========\n\n";
    
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
    
    std::cout << "\nAggregate Results:\n";
    std::cout << "==================\n";
    std::cout << "Final counter:    " << shared_data.counter << "\n";
    std::cout << "Expected:         " << (NUM_THREADS * ITERATIONS) << "\n";
    std::cout << "end_success:      " << thread_end_success.load() << "\n";
    std::cout << "end_fail:         " << thread_end_fail.load() << "\n";
    std::cout << "Total commits:    " << (thread_end_success.load() + thread_end_fail.load()) << "\n";
    std::cout << "Time elapsed:     " << duration.count() << " ms\n";
    
    // Verify that successful commits = expected counter
    if (thread_end_success.load() == NUM_THREADS * ITERATIONS) {
        std::cout << "PASS: Commits = expected\n";
    } else {
        std::cout << "FAIL: Commits mismatch\n";
    }
    
    int expected_counter = NUM_THREADS * ITERATIONS;
    if (shared_data.counter == expected_counter) {
        std::cout << "TEST PASSED\n";
        return 0;
    } else {
        std::cout << "TEST FAILED\n";
        std::cout << "Lost updates: " << (expected_counter - shared_data.counter) << "\n";
        return 1;
    }
}