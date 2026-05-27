// Minimal reproducer for STMbench7 bad_alloc crash.
// Replicates AtomicPart pattern: struct with std::vector<int> member,
// global vector, push_back inside TX (causes realloc + element move).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tm_test_common.hpp"

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

TM std::vector<AtomicPart> g_parts;

TX void push_tx() {
    g_parts.push_back(AtomicPart(999999, 1));
}

MAIN int main(int argc, char *argv[]) {
    int n = (argc > 1) ? atoi(argv[1]) : 10000;

    g_parts.resize(n);
    for (int i = 0; i < n; i++)
        g_parts[i] = AtomicPart(i, i % 5);
    printf("init %d elems, sizeof=%zu\n", n, sizeof(AtomicPart));

    fflush(stdout);
    push_tx();
    fflush(stdout);

    printf("done, size=%zu\n", g_parts.size());
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
    printf("ALL OK\n");
    return 0;
}
