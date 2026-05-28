#include "../../benchmarks/datastructures/tm_treap_map.hpp"
#include <cstdio>
#include "tm_test_common.hpp"

TM TMTreapMap<int, int> g_map;

TX void tx_insert_one(int key, int val) {
    g_map[key] = val;
}

TX int tx_find_one(int key) {
    auto it = g_map.find(key);
    if (it != g_map.end()) return it->second;
    return -1;
}

MAIN int main() {
    for (int i = 0; i < 10; i++) {
        g_map[i] = i * 10;
    }
    tx_insert_one(100, 0);
    int v = tx_find_one(100);
    if (v != 0) {
        printf("FAIL: g_map[100] = %d (expected 0)\n", v);
        return 1;
    }
    printf("Result: PASS\n");
    return 0;
}
