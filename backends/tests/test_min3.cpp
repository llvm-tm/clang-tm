/**
 * 2-thread test with timeout
 */
#include <stdio.h>
#include <thread>
#include <chrono>
extern "C" {
#include "../TinySTM/include/stm.h"
}

volatile int counter = 0;
std::atomic<int> started{0};

void tx_thread(int id) {
    stm_init_thread();
    
    started.fetch_add(1);
    if (started.load() == 1) {
        // brief pause to let both start
    }
    
    for (int i = 0; i < 3; i++) {
        printf("T%d iter %d start\n", id, i);
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL && sigsetjmp(*env, 0) == 0) {
            stm_word_t val = stm_load((volatile stm_word_t*)&counter);
            printf("T%d iter %d read %lu\n", id, i, (unsigned long)val);
            val++;
            stm_store((volatile stm_word_t*)&counter, val);
            
            if (stm_commit()) {
                printf("T%d iter %d COMMIT\n", id, i);
            } else {
                printf("T%d iter %d abort\n", id, i);
            }
        }
        printf("T%d iter %d done\n", id, i);
    }
    printf("T%d exiting\n", id);
    stm_exit_thread();
}

int main() {
    printf("Init\n");
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(tx_thread, 0);
    threads.emplace_back(tx_thread, 1);
    
    // wait with timeout
    auto done = threads[0].joinable() || threads[1].joinable();
    
    if (threads[0].joinable())
        threads[0].join();
    if (threads[1].joinable())  
        threads[1].join();
    
    printf("Final: %d\n", counter);
    stm_exit();
    return 0;
}