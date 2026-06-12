/**
 * Persistent KV Store — std::map with PSTATIC_REBUILD
 *
 * Uses TM-annotated arrays as backing store and automatically
 * rebuilds the std::map from them after each restart.
 *
 * The PSTATIC_REBUILD annotation tells the plugin to call this
 * function after tm_init() restores the TM-annotated arrays.
 * The function is also TX, so it runs inside a transaction
 * (allocations go to the persistent heap via tm_malloc).
 */

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define PSTATIC_REBUILD __attribute__((annotate("pstatic_rebuild"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

#include <cstdio>
#include "tm_safe_map.hpp"

constexpr int MAX_ENTRIES = 1024;

// Persistent backing store (TM-annotated → survives restart)
TM int      g_pkeys[MAX_ENTRIES];
TM int      g_pvals[MAX_ENTRIES];
TM int      g_pcount = 0;

// TM-safe map (allocations use ::operator new, redirected by LLVM pass)
static TMSafeMap<int, int> g_map;

// Called automatically after tm_init() restores persistent data.
// TX ensures it runs inside a transaction (allocations → persistent heap).
TX PSTATIC_REBUILD void rebuild_map() {
    g_map.clear();
    for (int i = 0; i < g_pcount; i++)
        g_map[g_pkeys[i]] = g_pvals[i];
}

TX void insert(int k, int v) {
    g_map[k] = v;
    for (int i = 0; i < g_pcount; i++) {
        if (g_pkeys[i] == k) {
            g_pvals[i] = v;
            return;
        }
    }
    int n = g_pcount;
    g_pkeys[n] = k;
    g_pvals[n] = v;
    g_pcount = n + 1;
}

TX int lookup(int k) {
    auto it = g_map.find(k);
    return it == g_map.end() ? -1 : it->second;
}

TX void show() {
    printf("  map: %zu entries, persistent: %d entries\n", g_map.size(), g_pcount);
    int n = 0;
    for (auto& [k, v] : g_map) {
        printf("    %d → %d\n", k, v);
        if (++n >= 5) { printf("    ...\n"); break; }
    }
}

MAIN int main() {
    printf("=== Persistent std::map with PSTATIC_REBUILD ===\n");

    int prev = lookup(42);
    printf("Previous value for key 42: %d\n", prev);

    insert(42, prev + 1);
    insert(prev, prev * 10);

    printf("After insertion:\n");
    show();

    // Clear the map before tm_exit() unmaps the persistent heap.
    // Otherwise the std::map destructor (which runs after main)
    // would try to free nodes on the unmapped mmap → segfault.
    g_map.clear();
    return 0;
}
