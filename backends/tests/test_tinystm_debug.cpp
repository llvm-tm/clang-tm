/**
 * Simple TinySTM Debug Test
 */

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

extern "C" {
#include "../TinySTM/include/stm.h"
}

std::atomic<int> ready{0};
std::atomic<int> done{0};

volatile int shared_counter = 0;

void simple_thread_func(int thread_id) {
    std::cout << "Thread " << thread_id << " starting...\n";
    
    stm_init_thread();
    std::cout << "Thread " << thread_id << " inited thread\n";
    
    ready.fetch_add(1);
    while (ready.load() < 2) { std::this_thread::yield(); }
    
    std::cout << "Thread " << thread_id << " starting tx...\n";
    
    for (int i = 0; i < 100; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        
        if (env != NULL) {
            if (sigsetjmp(*env, 0) == 0) {
                std::cout << "Thread " << thread_id << " read/write...\n";
                int val = shared_counter;
                val++;
                shared_counter = val;
                
                if (stm_commit()) {
                    std::cout << "Thread " << thread_id << " committed\n";
                } else {
                    std::cout << "Thread " << thread_id << " aborted\n";
                }
            }
        }
        std::cout << "Thread " << thread_id << " iter " << i << " done\n";
    }
    
    std::cout << "Thread " << thread_id << " exiting...\n";
    stm_exit_thread();
    done.fetch_add(1);
    std::cout << "Thread " << thread_id << " exit done\n";
}

int main() {
    std::cout << "Initializing TinySTM...\n";
    stm_init();
    std::cout << "TinySTM initialized\n";
    
    std::vector<std::thread> threads;
    threads.emplace_back(simple_thread_func, 0);
    threads.emplace_back(simple_thread_func, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    stm_exit();
    
    std::cout << "Final counter: " << shared_counter << " (expected: 200)\n";
    return 0;
}