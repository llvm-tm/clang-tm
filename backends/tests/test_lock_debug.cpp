/**
 * TinySTM lock CAS debug
 */

#include <iostream>
#include <thread>
#include <atomic>
#include <cstdio>

extern "C" {
#include "../TinySTM/include/stm.h"
}

// Instrumented STM
std::atomic<int> start_count{0};
volatile int counter = 0;

// Override with instrumentation
void thread_with_debug(int id) {
    stm_init_thread();
    
    start_count.fetch_add(1);
    while (start_count.load() < 2) {} // wait for both
    
    for (int i = 0; i < 2; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL && sigsetjmp(*env, 0) == 0) {
            // Use TinySTM transactional operations!
            stm_word_t val = stm_load((volatile stm_word_t*)&counter);
            val++;
            stm_store((volatile stm_word_t*)&counter, val);
            
            if (stm_commit()) {
                printf("T%d SUCCESS val=%lu\n", id, (unsigned long)val);
            } else {
                printf("T%d ABORTED\n", id);
            }
        }
    }
    stm_exit_thread();
}

int main() {
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(thread_with_debug, 0);
    threads.emplace_back(thread_with_debug, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    printf("Final: %d\n", counter);
    stm_exit();
    return 0;
}