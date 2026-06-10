#include <cstdio>
#include <map>

#include "tm_test_common.hpp"

// NOTE: This test is REFERENCE ONLY — NOT BUILT BY DEFAULT.  The plugin no
// longer supports std::map inside TM transactions.  std::map uses opaque
// _Rb_tree functions that the opaque checker flags as unsafe with TM-traced
// pointer args.  Removed from TEST_NAMES.

TM std::map<int, int> g_map;

TX void map_find_tx() {
    g_map[1] = 10;
    g_map[2] = 20;
    g_map[3] = 30;

    auto it = g_map.find(2);
    if (it == g_map.end()) return;

    it = g_map.find(42);
    if (it != g_map.end()) return;

    auto lb = g_map.lower_bound(2);
    if (lb == g_map.end()) return;

    auto ub = g_map.upper_bound(2);
    if (ub == g_map.end()) return;

    auto pos = g_map.find(1);
    if (pos != g_map.end()) {
        g_map.erase(pos);
    }
}

MAIN int main() {
    printf("std::map::find regression test\n");
    printf("==============================\n\n");

    g_map.clear();
    map_find_tx();

    printf("  g_map.size() = %zu\n", g_map.size());
    printf("  PASS: map find test passed\n");
    return 0;
}
