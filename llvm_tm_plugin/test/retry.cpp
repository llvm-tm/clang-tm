/**
 * Transaction Retry Test using longjmp/sigsetjmp
 *
 * This test verifies that:
 * 1. The plugin's injected sigsetjmp is working
 * 2. longjmp correctly jumps back to retry the transaction
 * 3. The return value is checked to avoid infinite loops
 * 4. Nested transactions can trigger retry to outermost
 */

#include <cstdio>
#include <cstdint>
#include <csetjmp>
#include <unistd.h>
#include <stdlib.h>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

extern "C" {
    extern __thread unsigned char tm_jmpbuf[256];
    extern __thread int32_t tm_longjmp_ret;
    extern __thread int32_t tm_nested_call_counter;
}

TM int32_t tm_counter = 0;
TM int32_t tm_max_retries = 3;

TX void retry_transaction() {
    printf("retry_transaction: start, counter=%d, jmpbuf_ret=%d, nested_counter=%d\n", 
           tm_counter, tm_longjmp_ret, tm_nested_call_counter);

    if (tm_longjmp_ret != 0) {
        printf("retry_transaction: retry detected! jmpbuf_ret=%d\n", tm_longjmp_ret);
        if (tm_longjmp_ret >= tm_max_retries) {
            printf("retry_transaction: ERROR - infinite loop! jmpbuf_ret=%d >= %d\n", 
                   tm_longjmp_ret, tm_max_retries);
            return;
        }
    }

    tm_counter++;
    printf("retry_transaction: incremented counter to %d\n", tm_counter);

    if (tm_counter < tm_max_retries) {
        printf("retry_transaction: triggering longjmp with value %d\n", tm_counter);
        longjmp(*(sigjmp_buf*)tm_jmpbuf, tm_counter);
    }

    printf("retry_transaction: committed! counter=%d\n", tm_counter);
}

TX void outer_retry_transaction() {
    printf("outer_retry_transaction: start, counter=%d\n", tm_counter);
    retry_transaction();
    printf("outer_retry_transaction: done, counter=%d\n", tm_counter);
}

int main() {
    printf("=== Test: Transaction retry with longjmp ===\n");

    tm_counter = 0;
    outer_retry_transaction();

    printf("main: final counter = %d (expected %d)\n", tm_counter, tm_max_retries);

    if (tm_counter != tm_max_retries) {
        printf("FAILED: expected counter=%d, got %d\n", tm_max_retries, tm_counter);
        return 1;
    }

    printf("Test PASSED!\n");
    fflush(stdout);
    _exit(0);
    return 0;
}