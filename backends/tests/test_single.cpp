/**
 * Single-thread TinySTM Test
 */

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

extern "C" {
#include "../TinySTM/include/stm.h"
}

int main() {
    std::cout << "Single-thread test\n";
    
    stm_init();
    std::cout << "Initialized\n";
    
    stm_init_thread();
    std::cout << "Thread inited\n";
    
    volatile int counter = 0;
    
    for (int i = 0; i < 10; i++) {
        sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
        if (env != NULL) {
            if (sigsetjmp(*env, 0) == 0) {
                counter++;
                if (stm_commit()) {
                    std::cout << "Commit " << i << ": " << counter << "\n";
                }
            }
        }
    }
    
    std::cout << "Final: " << counter << "\n";
    
    stm_exit_thread();
    stm_exit();
    
    return 0;
}