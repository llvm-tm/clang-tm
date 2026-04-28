/**
 * Check start/end timestamps
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

void check_timestamp(int id) {
    stm_init_thread();
    ready.fetch_add(1);
    while (ready.load() < 2) {}
    
    for (int i = 0; i < 2; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL && sigsetjmp(*env, 0) == 0) {
            int before = counter;
            counter = before + 1;
            
            if (stm_commit()) {
                // Note: we can't easily get start/end without more instrumentation
                printf("Thread %d C%dfinal=%d ", id, i, counter);
            } else {
                printf("Thread %d A%d ", id, i);
            }
        }
    }
    stm_exit_thread();
}

int main() {
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(check_timestamp, 0);
    threads.emplace_back(check_timestamp, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    printf("Final: %d\n", counter);
    stm_exit();
    return 0;
}