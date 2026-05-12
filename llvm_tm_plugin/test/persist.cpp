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

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

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