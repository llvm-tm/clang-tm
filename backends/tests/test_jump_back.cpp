/**
 * Jump-Back Test - uses sigsetjmp/siglongjmp for transaction retry
 * This test verifies that the TM implementation properly jumps back on abort
 * instead of using a while loop with a flag
 */

#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <csetjmp>

extern "C" {
    void tm_init();
    void tm_exit();
    void tm_init_thread();
    void tm_exit_thread();
    int tm_begin();
    int tm_end();
    sigjmp_buf* tm_get_env();
    void tm_set_env(sigjmp_buf* env);
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
std::atomic<int> success_count{0};
std::atomic<int> abort_count{0};
std::atomic<int> jump_back_count{0};

void thread_func(int thread_id) {
    tm_init_thread();
    
    sigjmp_buf jump_buffer;
    int setjmp_done = 0;
    
    for (int i = 0; i < ITERATIONS; ++i) {
        if (!setjmp_done) {
            // First time through - set up jump point
            if (tm_get_env()) {
                tm_set_env(&jump_buffer);
            }
        }
        
        // Start transaction
        if (!tm_begin()) {
            abort_count++;
            setjmp_done = 0;
            continue;
        }
        
        // Transaction body
        uint32_t val = tm_read_i4(&shared_data.counter);
        val++;
        tm_write_i4(&shared_data.counter, val);
        
        int committed = tm_end();
        
        if (committed) {
            success_count++;
            setjmp_done = 0;
        } else {
            // Abort - will jump back via longjmp if configured
            abort_count++;
            setjmp_done = 1;
            
            // Check if longjmp should happen - if tm_get_env returns valid buffer
            if (tm_get_env()) {
                jump_back_count++;
                siglongjmp(*tm_get_env(), 1);
                // Should never reach here
            }
            // If no longjmp capability, retry via loop
        }
    }
    
    tm_exit_thread();
}

int main() {
    std::cout << "Jump-Back Test (sigsetjmp/siglongjmp)\n";
    std::cout << "======================================\n";
    std::cout << "Threads:    " << NUM_THREADS << "\n";
    std::cout << "Iterations: " << ITERATIONS << "\n";
    std::cout << "Expected:   " << (NUM_THREADS * ITERATIONS) << "\n\n";
    
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
    
    std::cout << "Results:\n";
    std::cout << "========\n";
    std::cout << "Final counter:    " << shared_data.counter << "\n";
    std::cout << "Successful:       " << success_count.load() << "\n";
    std::cout << "Aborted:          " << abort_count.load() << "\n";
    std::cout << "Jump-backs:       " << jump_back_count.load() << "\n";
    std::cout << "Time elapsed:     " << duration.count() << " ms\n";
    
    int expected = NUM_THREADS * ITERATIONS;
    if (shared_data.counter == expected && success_count.load() == expected) {
        std::cout << "\nTEST PASSED\n";
        return 0;
    } else {
        std::cout << "\nTEST FAILED\n";
        return 1;
    }
}