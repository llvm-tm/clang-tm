#include <cstdio>
#include <cassert>
#include <map>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM std::map<int, int> g_map;

TX int find_and_read(int key) {
    auto it = g_map.find(key);
    if (it == g_map.end())
        return -1;
    return it->second;
}

MAIN int main() {
    g_map[42] = 100;
    g_map[7]  = 200;

    int val = find_and_read(42);
    printf("find(42) = %d (expected 100)\n", val);
    assert(val == 100);

    val = find_and_read(7);
    printf("find(7) = %d (expected 200)\n", val);
    assert(val == 200);

    val = find_and_read(99);
    printf("find(99) = %d (expected -1)\n", val);
    assert(val == -1);

    printf("PASS: map find + it->second test passed\n");
    return 0;
}
