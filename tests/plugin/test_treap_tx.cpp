#include "tm_treap_map.hpp"
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
    printf("A\n");
    fflush(stdout);
    for (int i = 0; i < 10; i++) {
        printf("B i=%d\n", i);
        fflush(stdout);
        g_map[i] = i * 10;
        printf("C i=%d\n", i);
        fflush(stdout);
    }
    printf("D\n");
    fflush(stdout);
    for (int i = 0; i < 10; i++) {
        printf("E i=%d\n", i);
        fflush(stdout);
        int v = g_map[i];
        printf("F i=%d v=%d\n", i, v);
        fflush(stdout);
        if (v != i * 10) {
            printf("FAIL: pre-populated g_map[%d] = %d (expected %d)\n", i, v, i * 10);
            return 1;
        }
    }
    printf("G\n");
    fflush(stdout);
    tx_insert_many();
    printf("H\n");
    fflush(stdout);
    for (int i = 0; i < 10; i++) {
        tx_check(100 + i, i);
    }
    printf("I\n");
    fflush(stdout);
    for (int i = 0; i < 10; i++) {
        tx_check(i, i * 10);
    }
    printf("J\n");
    fflush(stdout);
    printf("Result: PASS\n");
    return 0;
}
