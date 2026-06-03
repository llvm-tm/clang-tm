/// Test: queue-based synchronous transaction execution
/// Sync TXes (transaction annotation) are enqueued and the caller
/// automatically waits via plugin-inserted tm_wait_prev_tx().

#include <cstdio>

#include "tm_test_common.hpp"

static int counter = 0;

TX void add_one() {
    counter++;
}

TX void add_two() {
    counter += 2;
}

MAIN int main() {
    counter = 0;
    add_one();
    add_two();
    add_one();
    int expected = 4;
    if (counter == expected) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL: counter=%d expected=%d\n", counter, expected);
        return 1;
    }
}
