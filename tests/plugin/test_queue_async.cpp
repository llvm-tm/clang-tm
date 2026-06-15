/// Test: queue-based asynchronous transaction execution
/// Async TXes (async_shared annotation) are enqueued without
/// automatic wait.  The caller must call tm_wait_prev_tx() explicitly
/// before reading values.

#include <cstdio>

#include "tm_test_common.hpp"

extern "C" void tm_wait_prev_tx(void);

static int counter = 0;

__attribute__((noinline, annotate("async_shared")))
void async_add_one() {
    counter++;
}

MAIN int main() {
    counter = 0;
    async_add_one();
    async_add_one();
    tm_wait_prev_tx();
    int expected = 2;
    if (counter == expected) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL: counter=%d expected=%d\n", counter, expected);
        return 1;
    }
}
