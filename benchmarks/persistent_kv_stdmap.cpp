/**
 * Persistent KV Store — std::map with explicit serialization
 *
 * The challenge with std::map persistence is pointer relocation:
 * tree nodes contain absolute addresses of other nodes, which become
 * stale when the memory region moves between runs.
 *
 * Three approaches to heap-data persistence:
 *
 *   (a) Serialization  ← this file
 *       Walk the data structure, save key-value pairs to a TM-annotated
 *       array, restore them on next init.  Works for any container but
 *       needs explicit save/load logic.
 *
 *   (b) Custom allocator with fixed-address mmap
 *       std::map allocates from an mmap region mapped at a deterministic
 *       virtual address.  Nodes survive restart because the mapping is
 *       at the same address.  Fragile (ASLR, address conflicts).
 *
 *   (c) malloc/free instrumentation
 *       Replace malloc/free with persistent mmap allocations.
 *       Transparent to application code but requires LD_PRELOAD or
 *       deep compiler support, and still faces the pointer-relocation
 *       problem unless the mmap base address is fixed.
 *
 * We implement (a):
 *   - A TM-annotated array stores (key, value) pairs persistently
 *   - A TM integer tracks the entry count
 *   - On init, the std::map is populated from the array
 *   - On each mutation, the array is updated atomically
 *   - On restart, the array is restored from the persist file,
 *     then the std::map is rebuilt from it
 *
 * Build: see benchmarks/persistent_kv.cpp for the pipeline.
 * Run:   rm -f tm_persist.bin && ./bin/persistent_kv_stdmap
 *        ./bin/persistent_kv_stdmap   # persists!
 */

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

#include <cstdio>
#include <cstdint>
#include <map>

constexpr int MAX_ENTRIES = 1024;

// Persistent backing store (TM-annotated → survives restart)
TM int      g_pkeys[MAX_ENTRIES];
TM int      g_pvals[MAX_ENTRIES];
TM int      g_pcount = 0;

// In-memory std::map (rebuilt from persistent store on each init)
static std::map<int, int> g_map;

static void rebuild_map() {
    g_map.clear();
    for (int i = 0; i < g_pcount; i++)
        g_map[g_pkeys[i]] = g_pvals[i];
}

TX void insert(int k, int v) {
    // Update std::map
    g_map[k] = v;
    // Persist to TM-annotated array
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
    printf("  map size: %zu, persistent entries: %d\n", g_map.size(), g_pcount);
    int n = 0;
    for (auto& [k, v] : g_map) {
        printf("    %d → %d\n", k, v);
        if (++n >= 5) { printf("    ...\n"); break; }
    }
}

MAIN int main() {
    printf("=== Persistent std::map KV Store (serialized) ===\n");

    // Rebuild std::map from persistent backing store
    rebuild_map();

    int prev = lookup(42);
    printf("Previous value for key 42: %d\n", prev);

    insert(42, prev + 1);
    insert(prev, prev * 10);

    printf("After insertion:\n");
    show();
    return 0;
}
