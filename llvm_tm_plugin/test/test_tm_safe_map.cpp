#include "../../backends/tm_safe_map.hpp"
#include <cstdio>
#include <cstdint>

#include "tm_test_common.hpp"

TM TMSafeMap<int64_t, int64_t> g_map;

TX void test_inserts() {
    g_map[1] = 10;
    g_map[2] = 20;
    g_map[3] = 30;
}

MAIN int main() {
    test_inserts();
    printf("g_map.size() = %zu\n", g_map.size());
    for (auto &kv : g_map) {
        printf("  g_map[%lld] = %lld\n", (long long)kv.first, (long long)kv.second);
    }
    bool ok = true;
    if (g_map[1] != 10) { printf("FAIL: g_map[1] = %lld (expected 10)\n", (long long)g_map[1]); ok = false; }
    if (g_map[2] != 20) { printf("FAIL: g_map[2] = %lld (expected 20)\n", (long long)g_map[2]); ok = false; }
    if (g_map[3] != 30) { printf("FAIL: g_map[3] = %lld (expected 30)\n", (long long)g_map[3]); ok = false; }
    if (ok) printf("Result: PASS\n");
    return ok ? 0 : 1;
}
