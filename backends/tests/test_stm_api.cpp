/**
 * TinySTM proper test with stm_load/stm_store
 */

#include <iostream>
#include <thread>
#include <atomic>
#include <cstdio>

extern "C" {
#include "../TinySTM/include/stm.h"
}

std::atomic<int> ready{0};
volatile int counter = 0;

void thread_with_stm(int id) {
    stm_init_thread();
    ready.fetch_add(1);
    while (ready.load() < 2) {}
    
    for (int i = 0; i < 2; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL) {
            if (sigsetjmp(*env, 0) == 0) {
                // TinySTM transactional operations
                stm_word_t val = stm_load((volatile stm_word_t*)&counter);
                val++;
                stm_store((volatile stm_word_t*)&counter, val);
                
                if (stm_commit()) {
                    printf("T%d SUCCESS\n", id);
                } else {
                    printf("T%d commit failed, retrying\n", id);
                }
            } else {
                // Abort and restart
                printf("T%d aborted, restarting\n", id);
            }
        }
    }
    stm_exit_thread();
}

int main() {
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(thread_with_stm, 0);
    threads.emplace_back(thread_with_stm, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    printf("Final: %d\n", counter);
    stm_exit();
    return 0;
}