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

void worker_thread(int thread_id) {
    printf("worker_thread %d: starting\n", thread_id);
    fflush(stdout);
    
    // Perform a few transactions to show hooks are called
    for (int i = 0; i < 2; i++) {
        increment_counter();
    }
    
    printf("worker_thread %d: finished\n", thread_id);
    fflush(stdout);
}

int main() {
    printf("tm_threads test: starting\n");
    fflush(stdout);
    
    // Create 2 worker threads
    const int num_threads = 2;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; i++) {
        threads.push_back(std::thread(worker_thread, i));
    }
    
    // Wait for all threads to complete
    for (auto &t : threads) {
        t.join();
    }
    
    printf("tm_threads test: all threads joined\n");
    printf("tm_threads test: PASSED\n");
    fflush(stdout);
    return 0;
}
