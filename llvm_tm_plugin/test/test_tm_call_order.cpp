#include <cstdio>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

// Shared transactional variable
TM int shared_counter = 0;

TX void increment_counter() {
    shared_counter = shared_counter + 1;
}

MAIN int main() {
    printf("tm_call_order test: starting\n");
    fflush(stdout);
    
    // Simple lambda thread - will create worker_func through std::thread
    std::thread t([]() {
        increment_counter();
    });
    t.join();
    
    printf("tm_call_order test: thread joined\n");
    printf("tm_call_order test: PASSED\n");
    fflush(stdout);
    return 0;
}