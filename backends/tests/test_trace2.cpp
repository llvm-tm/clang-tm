/**
 * TinySTM trace - 2 iterations
 */

#include <iostream>
#include <thread>
#include <atomic>

extern "C" {
#include "../TinySTM/include/stm.h"
}

std::atomic<int> ready{0};
volatile int counter = 0;

void trace_func(int id) {
    stm_init_thread();
    ready.fetch_add(1);
    while (ready.load() < 2) {}
    
    for (int i = 0; i < 2; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL && sigsetjmp(*env, 0) == 0) {
            int before = counter;
            counter = before + 1;
            
            if (stm_commit()) {
                std::printf("Thread %d iter %d: SUCCESS val=%d\n", id, i, counter);
            } else {
                std::printf("Thread %d iter %d: ABORTED\n", id, i);
            }
        }
    }
    
    stm_exit_thread();
}

int main() {
    stm_init();
    
    std::vector<std::thread> threads;
    threads.emplace_back(trace_func, 0);
    threads.emplace_back(trace_func, 1);
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::printf("Final: %d\n", counter);
    stm_exit();
    return 0;
}