// This is what the plugin produces for threads.cpp
// Instrumented version of: test/threads.cpp

#include <cstdio>
#include <thread>
#include <vector>
#include <csetjmp>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

// Shared transactional variable
TM int shared_counter = 0;

// Thread-local state
__thread int32_t tm_nested_call_counter = 0;
__thread unsigned char tm_jmpbuf[256];
__thread int32_t tm_jmpbuf_ret = 0;
__thread uint8_t is_tm_init_thread_ready = 0;

// Runtime hooks
void tm_init() { printf("tm_init\n"); }
void tm_exit() { printf("tm_exit\n"); }
void tm_init_thread() {
    if (is_tm_init_thread_ready == 0) {
        printf("tm_init_thread\n");
        is_tm_init_thread_ready = 1;
    }
}
void tm_exit_thread() {
    if (is_tm_init_thread_ready == 1) {
        printf("tm_exit_thread\n");
        is_tm_init_thread_ready = 0;
    }
}

void tm_begin() {
    if (tm_jmpbuf_ret == 0) { // not an abort
        printf("tm_nested_call_counter=%d  --  ", tm_nested_call_counter);
        if (tm_nested_call_counter == 1) {
            printf("tm_begin outer\n");
        } else {
            printf("tm_begin nested %d\n", tm_nested_call_counter);
        }
    }
}
void tm_end() {
    printf("tm_nested_call_counter=%d  --  ", tm_nested_call_counter);
    if (tm_nested_call_counter == 1) {
        printf("tm_end outer\n");
    } else {
        printf("tm_end nested %d\n", tm_nested_call_counter);
    }
}

int tm_read_i4(void* addr) { return *(int32_t*)addr; }
void tm_write_i4(void* addr, int32_t val) { *(int32_t*)addr = val; }

// Transaction function
void increment_counter() {
    tm_nested_call_counter++;

    // ===== ENTRY =====
    if (tm_nested_call_counter == 1) {
        // OUTER path - new transaction
        tm_jmpbuf_ret = sigsetjmp(*(sigjmp_buf*)tm_jmpbuf, 0);
        if (tm_jmpbuf_ret == 0) {
            tm_nested_call_counter = 1;
        }
        tm_begin();
    }
    // NESTED path: NO tm_begin() - only outermost calls tm_begin()

    // ===== BODY =====
    {
        int32_t tmp = tm_read_i4(&shared_counter);
        tm_write_i4(&shared_counter, tmp + 1);
    }

    // ===== EXIT =====
    if (tm_nested_call_counter == 1) {
        tm_end();
    }
    tm_nested_call_counter--;
}

// Worker thread function - this is NOT a transaction function!
// It uses TM globals (shared_counter) so it needs thread init/exit
void worker_thread(int thread_id) {
    // Thread entry point - inject tm_init_thread()
    if (is_tm_init_thread_ready == 0) {
        printf("tm_init_thread\n");
        tm_init_thread();
        is_tm_init_thread_ready = 1;
    }

    printf("worker_thread %d: starting\n", thread_id);
    
    // Perform a few transactions to show hooks are called
    for (int i = 0; i < 2; i++) {
        increment_counter();
    }
    
    printf("worker_thread %d: finished\n", thread_id);
    if (is_tm_init_thread_ready == 1) {
        tm_exit_thread();
        is_tm_init_thread_ready = 0;
    }
}

// Main function
int main() {
    tm_init();

    // Main thread initialization - must be BEFORE any transaction
    if (is_tm_init_thread_ready == 0) {
        tm_init_thread();
        is_tm_init_thread_ready = 1;
    }

    printf("tm_threads test: starting\n");

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
    
    // Main thread cleanup
    if (is_tm_init_thread_ready == 1) {
        tm_exit_thread();
        is_tm_init_thread_ready = 0;
    }

    printf("tm_threads test: PASSED\n");
    tm_exit();
    return 0;
}