/**
 * TinySTM trace - single iteration
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
    
    std::printf("Thread %d start\n", id);
    
    sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
    if (env != NULL && sigsetjmp(*env, 0) == 0) {
        int before = counter;
        std::printf("Thread %d read=%d\n", id, before);
        counter = before + 1;
        std::printf("Thread %d write=%d\n", id, counter);
        
        if (stm_commit()) {
            std::printf("Thread %d COMMIT SUCCESS\n", id);
        } else {
            std::printf("Thread %d COMMIT FAILED\n", id);
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