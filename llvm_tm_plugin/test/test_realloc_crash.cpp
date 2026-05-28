// Minimal reproducer for STMbench7 bad_alloc crash.
// Replicates: struct with inner vector<int>, global vector push_back
// inside TX + inner vector push_back + treap map update.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tm_test_common.hpp"

#include "../../benchmarks/datastructures/tm_treap_map.hpp"

struct AtomicPart {
    int32_t id;
    int32_t type;
    int32_t x, y, vx, vy;
    std::vector<int> ids;

    AtomicPart() : id(0), type(0), x(0), y(0), vx(0), vy(0) {}
    AtomicPart(int i, int t) : id(i), type(t), x(0), y(0), vx(0), vy(0) {
        ids.push_back(i);
    }
};

struct CompositePart {
    int32_t id;
    std::vector<int> atomicPartIds;
};

TM std::vector<AtomicPart>    g_parts;
TM std::vector<CompositePart> g_composites;
TM TMTreapMap<int,int>        g_index;

TX void push_tx() {
    int idx = (int)g_parts.size();
    g_parts.push_back(AtomicPart(999999, 1));
    g_index[idx] = (int)g_parts.size() - 1;
    g_composites[0].atomicPartIds.push_back(idx);
}

MAIN int main(int argc, char *argv[]) {
    int n = (argc > 1) ? atoi(argv[1]) : 500;

    g_composites.resize(1);
    g_composites[0].id = 0;
    g_composites[0].atomicPartIds.resize(n);
    for (int i = 0; i < n; i++)
        g_composites[0].atomicPartIds[i] = i;

    g_parts.resize(n);
    for (int i = 0; i < n; i++)
        g_parts[i] = AtomicPart(i, i % 5);
    printf("init %d elems, sizeof(AP)=%zu sizeof(CP)=%zu\n", n, sizeof(AtomicPart), sizeof(CompositePart));

    fflush(stdout);
    push_tx();
    fflush(stdout);

    printf("done, parts=%zu composites[0].ids=%zu index.size=%zu\n",
           g_parts.size(), g_composites[0].atomicPartIds.size(), g_index.size());
    fflush(stdout);

    // Verify
    for (int i = 0; i < (int)g_parts.size() - 1; i++) {
        if (g_parts[i].id != i) {
            printf("FAIL: id mismatch at %d: got %d\n", i, g_parts[i].id);
            return 1;
        }
    }
    if (g_parts.back().id != 999999) {
        printf("FAIL: last element id mismatch: %d\n", g_parts.back().id);
        return 1;
    }
    if (g_composites[0].atomicPartIds.size() != (size_t)n + 1) {
        printf("FAIL: expected %d inner elements, got %zu\n", n + 1, g_composites[0].atomicPartIds.size());
        return 1;
    }
    if (g_composites[0].atomicPartIds.back() != n) {
        printf("FAIL: last inner element: expected %d got %d\n", n, g_composites[0].atomicPartIds.back());
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
