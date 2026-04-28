/**
 * TinySTM debug with detailed trace
 */

#include <stdio.h>
#include <thread>
#include <atomic>
extern "C" {
#include "../TinySTM/include/stm.h"
}

std::atomic<int> started{0};
volatile int counter = 0;

void tx_thread(int id) {
    stm_init_thread();
    started.fetch_add(1);
    while (started.load() < 2) {}
    
    for (int i = 0; i < 10; i++) {
        printf("T%d: iter %d stm_start\n", id, i);
        sigjmp_buf *jmpbuf = stm_start((stm_tx_attr_t){0});
        
        if (jmpbuf != NULL) {
            printf("T%d: top-level, sigsetjmp\n", id);
            if (sigsetjmp(*jmpbuf, 0) == 0) {
                printf("T%d: read\n", id);
                stm_word_t val = stm_load((volatile stm_word_t*)&counter);
                val++;
                printf("T%d: write %lu\n", id, (unsigned long)val);
                stm_store((volatile stm_word_t*)&counter, val);
                
                int r = stm_commit();
                printf("T%d: commit returned %d\n", id, r);
            } else {
                printf("T%d: sigsetjmp returned non-zero (abort)\n", id);
            }
        } else {
            printf("T%d: nested\n", id);
            // Nested transaction - shouldn't happen in this test
        }
        
        printf("T%d: iter %d done\n", id, i);
    }
    printf("T%d: exit\n", id);
    stm_exit_thread();
}

int main() {
    printf("Main: init\n");
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(tx_thread, 0);
    threads.emplace_back(tx_thread, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    printf("Final: %d\n", counter);
    stm_exit();
    return 0;
}