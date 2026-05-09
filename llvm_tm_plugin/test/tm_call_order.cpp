#include <cstdio>
#include <thread>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

// Shared transactional variable
TM int shared_counter = 0;

TX void increment_counter() {
    shared_counter = shared_counter + 1;
}

int main() {
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