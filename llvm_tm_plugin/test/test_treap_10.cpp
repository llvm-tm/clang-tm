#include "../../benchmarks/datastructures/tm_treap_map.hpp"
#include <cstdio>
#include "tm_test_common.hpp"

TM TMTreapMap<int, int> g_map;

TX void tx_insert_10() {
    for (int i = 0; i < 10; i++) {
        g_map[100 + i] = i;
    }
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
    tx_insert_10();
    for (int i = 0; i < 10; i++) {
        int v = tx_find_one(100 + i);
        if (v != i) {
            printf("FAIL: g_map[%d] = %d (expected %d)\n", 100 + i, v, i);
            return 1;
        }
    }
    printf("Result: PASS\n");
    return 0;
}
