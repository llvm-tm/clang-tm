/**
 * Persistence Test
 * 
 * This test verifies that TM variables persist between runs.
 * Run: ./persist_test
 * Run again: ./persist_test
 * The counter should continue incrementing.
 */

#include <cstdio>
#include <cstdint>

#include "tm_test_common.hpp"

TM int32_t counter = 0;

TX void increment() {
    printf("increment: counter before = %d\n", counter);
    counter++;
    printf("increment: counter after = %d\n", counter);
}

MAIN int main() {
    printf("main: starting, counter = %d\n", counter);
    increment();
    increment();
    printf("main: done, final counter = %d\n", counter);
    return 0;
}