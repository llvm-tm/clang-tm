#include <cstdio>
#include <thread>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM int g_counter = 0;

TX void inc() { g_counter = g_counter + 1; }

THREAD void worker() { for (int i = 0; i < 100000; i++) inc(); }

MAIN int main() {
    printf("counter_race: start\n"); fflush(stdout);
    std::thread t1(worker);
    std::thread t2(worker);
    t1.join(); t2.join();
    printf("counter_race: g_counter=%d (expected 200000)\n", g_counter); fflush(stdout);
    return 0;
}
