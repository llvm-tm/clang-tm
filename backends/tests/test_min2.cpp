/**
 * 2-thread test
 */
#include <stdio.h>
#include <thread>
extern "C" {
#include "../TinySTM/include/stm.h"
}

volatile int counter = 0;
std::atomic<int> ready{0};

void tx_thread(int id) {
    stm_init_thread();
    ready.fetch_add(1);
    while (ready.load() < 2) {}
    
    for (int i = 0; i < 10; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL && sigsetjmp(*env, 0) == 0) {
            stm_word_t val = stm_load((volatile stm_word_t*)&counter);
            val++;
            stm_store((volatile stm_word_t*)&counter, val);
            
            if (stm_commit()) {
                printf("T%d C%d\n", id, i);
            } else {
                printf("T%d A%d\n", id, i);
            }
        }
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
    
    printf("Final: %d (expected 20)\n", counter);
    stm_exit();
    return 0;
}