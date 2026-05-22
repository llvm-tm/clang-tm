#include <map>
#include <cstdio>

#include "tm_test_common.hpp"

TM std::map<int, int> g_map;

TX int lookup(int key) {
    auto it = g_map.find(key);
    if (it == g_map.end()) return -1;
    return it->second;
}

MAIN int main() {
    for (int i = 0; i < 1000; i++)
        g_map[i] = i * 2;
    int r = lookup(500);
    printf("lookup(500) = %d (expected %d)\n", r, 1000);
    if (r == 1000) {
        printf("test_tm_map PASSED\n");
        return 0;
    }
    printf("test_tm_map FAILED\n");
    return 1;
}
