/**
 * Debug TinySTM with instrumented lock
 */

#include <iostream>
#include <thread>
#include <atomic>
#include <cstdio>

extern "C" {
#include "../TinySTM/include/stm.h"
}

std::atomic<int> ready{0};
std::atomic<int> check_lock{0};
volatile int counter = 0;

void thread_func(int id) {
    stm_init_thread();
    ready.fetch_add(1);
    while (ready.load() < 2) {}
    
    for (int i = 0; i < 10; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL) {
            if (sigsetjmp(*env, 0) == 0) {
                int c = counter;
                c++;
                counter = c;
                if (stm_commit()) {
                    // success
                } else {
                    // aborted
                }
            }
        }
        
        // Print progress
        if (i == 9) {
            std::printf("Thread %d done: %d\n", id, counter);
        }
    }
    stm_exit_thread();
}

int main() {
    std::printf("Starting...\n");
    
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(thread_func, 0);
    threads.emplace_back(thread_func, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::printf("Final: %d (expected 20)\n", counter);
    
    stm_exit();
    
    return 0;
}