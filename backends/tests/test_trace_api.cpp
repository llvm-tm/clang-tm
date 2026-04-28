/**
 * TinySTM trace test
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

void trace_thread(int id) {
    printf("T%d: init thread\n", id);
    stm_init_thread();
    
    printf("T%d: start loop\n", id);
    ready.fetch_add(1);
    while (ready.load() < 2) {}
    
    for (int i = 0; i < 2; i++) {
        printf("T%d: start tx %d\n", id, i);
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        
        if (env != NULL && sigsetjmp(*env, 0) == 0) {
            printf("T%d: load\n", id);
            stm_word_t val = stm_load((volatile stm_word_t*)&counter);
            
            printf("T%d: store %lu\n", id, (unsigned long)val);
            stm_store((volatile stm_word_t*)&counter, val + 1);
            
            printf("T%d: commit\n", id);
            if (stm_commit()) {
                printf("T%d: SUCCESS\n", id);
            } else {
                printf("T%d: FAILED\n", id);
            }
        }
    }
    printf("T%d: done\n", id);
    stm_exit_thread();
}

int main() {
    printf("main: init\n");
    stm_init();
    printf("main: done init\n");
    
    std::vector<std::thread> threads;
    threads.emplace_back(trace_thread, 0);
    threads.emplace_back(trace_thread, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    printf("Final: %d\n", counter);
    stm_exit();
    return 0;
}