/**
 * Debug TinySTM with abort tracking
 */

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>

extern "C" {
#include "../TinySTM/include/stm.h"
}

std::atomic<int> ready{0};
std::atomic<int> committed{0};
std::atomic<int> aborted{0};
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
                    committed.fetch_add(1);
                } else {
                    aborted.fetch_add(1);
                    // fall through to retry
                }
            } else {
                // restarted after abort
                aborted.fetch_add(1);
            }
        }
        
        if (i == 9) {
            std::printf("Thread %d: c=%d a=%d\n", id, committed.load(), aborted.load());
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
    
    std::printf("Final: %d committed=%d aborted=%d (expected 20 commits)\n", 
              counter, committed.load(), aborted.load());
    
    stm_exit();
    
    return 0;
}