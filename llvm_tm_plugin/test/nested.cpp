#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM int32_t tm_counter = 0;

TX void nested_tx() {
    tm_counter = tm_counter + 1;
}

TX void outer_tx() {
    tm_counter = tm_counter + 1;
    nested_tx();
    tm_counter = tm_counter + 1;
}

MAIN int main() {
    outer_tx();
    return 0;
}
