#include "../../plugin-benchmarks/datastructures/tm_treap_map.hpp"
#include <cstdio>
#include "tm_test_common.hpp"

TM TMTreapMap<int, int> g_map;

TX void tx_insert_many() {
    for (int i = 0; i < 10; i++) {
        g_map[100 + i] = i;
    }
}

TX int tx_find(int key) {
    auto it = g_map.find(key);
    if (it != g_map.end()) return it->second;
    return -1;
}

TX void tx_check(int key, int expected) {
    int v = tx_find(key);
    if (v != expected) {
        printf("FAIL: g_map[%d] = %d (expected %d)\n", key, v, expected);
        __builtin_trap();
    }
}

MAIN int main() {
    for (int i = 0; i < 10; i++) {
        g_map[i] = i * 10;
    }
    for (int i = 0; i < 10; i++) {
        int v = g_map[i];
        if (v != i * 10) {
            printf("FAIL: pre-populated g_map[%d] = %d (expected %d)\n", i, v, i * 10);
            return 1;
        }
    }
    tx_insert_many();
    for (int i = 0; i < 10; i++) {
        tx_check(100 + i, i);
    }
    for (int i = 0; i < 10; i++) {
        tx_check(i, i * 10);
    }
    printf("Result: PASS\n");
    return 0;
}
