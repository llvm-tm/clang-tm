// STMbench7-like: global vector<StructWithInnerVec>, push_back then read inner vec size
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

struct AtomicPart {
    int id;
    std::vector<int> connector_ids;  // inner vector
};

struct CompositePart {
    int id;
    std::vector<AtomicPart> atomic_parts;
};

// Global containers like STMbench7
std::vector<CompositePart> g_composite_parts;
std::vector<AtomicPart> g_atomic_parts;

__attribute__((annotate("transaction"), noinline))
void create_composite(int id, int n_atomic,
    std::vector<CompositePart>& composite_parts,
    std::vector<AtomicPart>& atomic_parts) {
    CompositePart cp;
    cp.id = id;
    
    // Create atomic parts
    for (int j = 0; j < n_atomic; j++) {
        AtomicPart ap;
        ap.id = id * 100 + j;
        ap.connector_ids = {j, j+1, j+2};
        cp.atomic_parts.push_back(ap);
    }
    
    // push_back to global (like STMbench7 g_compositeParts.push_back)
    composite_parts.push_back(cp);
    
    // Now read it back and verify sizes
    int idx = (int)composite_parts.size() - 1;
    auto& back = composite_parts[idx];
    
    // Read inner vectors (this is where STMbench7 crashes)
    for (int j = 0; j < n_atomic; j++) {
        auto& ap = back.atomic_parts[j];
        if (ap.connector_ids.empty()) {
            fprintf(stderr, "FAIL: empty connector_ids at idx=%d j=%d\n", idx, j);
            exit(1);
        }
    }
}

int main(int argc, char** argv) {
    int n_ops = argc > 1 ? atoi(argv[1]) : 100;
    printf("Testing STMbench7-like pattern with %d pushes\n", n_ops);

    for (int i = 0; i < n_ops; i++) {
        create_composite(i, 5, g_composite_parts, g_atomic_parts);
    }

    printf("g_composite_parts.size() = %zu (expected %d)\n",
           g_composite_parts.size(), n_ops);
    if (g_composite_parts.size() != (size_t)n_ops) {
        fprintf(stderr, "FAIL: wrong final size\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
