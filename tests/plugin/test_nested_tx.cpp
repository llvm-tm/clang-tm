#include <cstdio>

#include "tm_test_common.hpp"

TM int counter = 0;

TX void inner_tx() {
    printf("inner_tx: counter before = %d\n", counter);
    counter += 10;
    printf("inner_tx: counter after = %d\n", counter);
}

TX void outer_tx() {
    printf("outer_tx: counter before = %d\n", counter);
    counter += 1;
    inner_tx();
    counter += 100;
    printf("outer_tx: counter after = %d\n", counter);
}

MAIN int main() {
    printf("main: starting\n");
    outer_tx();
    printf("main: final counter = %d\n", counter);
    return 0;
}
