/**
 * Simple single-threaded test for debugging
 */
#include <stdio.h>
#include <stdint.h>

// Runtime stubs
extern "C" {
    void tm_init();
    void tm_exit();
    void tm_init_thread();
    void tm_exit_thread();
    int tm_begin();
    int tm_end();
    uint32_t tm_read_i4(volatile uint32_t* addr);
    void tm_write_i4(volatile uint32_t* addr, uint32_t val);
}

volatile uint32_t counter = 0;

int main() {
    printf("Single-threaded runtime test\n");
    
    tm_init();
    tm_init_thread();
    
    int iterations = 100;
    for (int i = 0; i < iterations; i++) {
        int committed = 0;
        while (!committed) {
            if (!tm_begin()) {
                printf("Abort before transaction start\n");
                continue;
            }
            uint32_t val = tm_read_i4(&counter);
            val++;
            tm_write_i4(&counter, val);
            committed = tm_end();
            if (!committed) {
                printf("Transaction %d aborted at commit\n", i);
            }
        }
    }
    
    printf("Final counter: %u (expected: %d)\n", counter, iterations);
    
    tm_exit_thread();
    tm_exit();
    
    if (counter == iterations) {
        printf("TEST PASSED\n");
        return 0;
    } else {
        printf("TEST FAILED\n");
        return 1;
    }
}
