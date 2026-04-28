/**
 * TinySTM with abort tracking 
 */

#include <stdio.h>
#include <thread>
#include <atomic>
extern "C" {
#include "../TinySTM/include/stm.h"
}

std::atomic<int> started{0};
volatile int counter = 0;
std::atomic<int> commits{0};
std::atomic<int> aborts{0};

void tx_thread(int id) {
    stm_init_thread();
    started.fetch_add(1);
    while (started.load() < 2) {}
    
    for (int i = 0; i < 100; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL && sigsetjmp(*env, 0) == 0) {
            stm_word_t val = stm_load((volatile stm_word_t*)&counter);
            val++;
            stm_store((volatile stm_word_t*)&counter, val);
            
            if (stm_commit()) {
                commits.fetch_add(1);
            } else {
                aborts.fetch_add(1);
            }
        }
        
        if (i % 10 == 0) {
            printf("T%d: c=%d a=%d\n", id, commits.load(), aborts.load());
        }
        
        if (commits.load() >= 4) break;  // enough
    }
    stm_exit_thread();
}

int main() {
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(tx_thread, 0);
    threads.emplace_back(tx_thread, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    printf("Final: %d commits=%d aborts=%d\n", counter, commits.load(), aborts.load());
    stm_exit();
    return 0;
}