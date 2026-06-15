// Minimal STMbench7-like reproducer: vector<StructWithVector> realloc inside TX
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

struct InnerVec {
    std::vector<int> ids;
};

struct OuterStruct {
    int id;
    InnerVec inner;
};

__attribute__((annotate("shared"), noinline))
void tx_work(std::vector<OuterStruct>& vec, int n, int thread_id) {
    for (int i = 0; i < n; i++) {
        // push_back triggers realloc (like g_compositeParts)
        vec.push_back(OuterStruct{thread_id * 1000 + i, InnerVec{}});
    }
    // Verify: read back elements and check inner vector is empty
    for (int i = 0; i < n; i++) {
        auto& elem = vec[i];
        if (!elem.inner.ids.empty()) {
            fprintf(stderr, "FAIL: inner vector not empty at i=%d\n", i);
            exit(1);
        }
        if (elem.id != thread_id * 1000 + i) {
            fprintf(stderr, "FAIL: wrong id at i=%d, got %d expected %d\n",
                    i, elem.id, thread_id * 1000 + i);
            exit(1);
        }
    }
    // Modify inner vector for one element (like atomicPartIds.push_back)
    for (int i = 0; i < n && i < 10; i++) {
        vec[i].inner.ids.push_back(i);
    }
    // Verify
    for (int i = 0; i < n && i < 10; i++) {
        if (vec[i].inner.ids.size() != 1 || vec[i].inner.ids[0] != i) {
            fprintf(stderr, "FAIL: inner vector wrong at i=%d, size=%zu val=%d\n",
                    i, vec[i].inner.ids.size(), vec[i].inner.ids[0]);
            exit(1);
        }
    }
}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 50;
    printf("Testing vector<StructWithVector> push_back with n=%d\n", n);

    std::vector<OuterStruct> vec;
    vec.reserve(16); // force early realloc

    tx_work(vec, n, 0);

    // Single-threaded post-TX verification
    printf("vec.size() = %zu (expected %d)\n", vec.size(), n);
    if (vec.size() != (size_t)n) {
        fprintf(stderr, "FAIL: wrong final size\n");
        return 1;
    }
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (vec[i].id == i) ok++;
        if (i < 10 && vec[i].inner.ids.size() == 1) ok++;
    }
    printf("Verified %d elements\n", ok);
    printf("PASS\n");
    return 0;
}
