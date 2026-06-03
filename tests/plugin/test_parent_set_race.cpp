#include <cstdio>
#include <set>
#include <thread>

#include "tm_test_common.hpp"

TM std::set<int> g_set;

TX void add(int v) { g_set.insert(v); }

TX bool find(int v) { return g_set.find(v) != g_set.end(); }

THREAD void adder() { for (int i = 0; i < 200000; i++) { add(i); add(i + 200000); } }

THREAD void searcher() { for (int i = 0; i < 200000; i++) find(i); }

MAIN int main() {
    printf("parent_set_race: start\n"); fflush(stdout);
    std::thread t1(adder);
    std::thread t2(searcher);
    std::thread t3(adder);
    std::thread t4(searcher);
    t1.join(); t2.join(); t3.join(); t4.join();
    printf("parent_set_race: done (size=%zu)\n", g_set.size()); fflush(stdout);
    return 0;
}
