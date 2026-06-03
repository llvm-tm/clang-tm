// test_region_alloc.cpp — Unit tests for stm::tm_region_allocator
//
// Standalone — no TM runtime dependency, just the region allocator.
//
// Tests:
//   init / destroy, malloc of various sizes (1 B … 1 MB),
//   16-byte alignment, isTMAddress(), calloc, realloc,
//   multi-threaded concurrent allocation, oversized (> slab size),
//   bump order (non-overlapping), zero-size, slab exhaustion.

#include "tm_region_allocator.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

// ── Minimal test macros ────────────────────────────────────────────
static int g_fails = 0;

#define T(cond, msg)                                                         \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
            g_fails++;                                                       \
        }                                                                    \
    } while (0)

#define TEQ(a, b, msg)                                                        \
    do {                                                                      \
        if ((a) != (b)) {                                                     \
            fprintf(stderr, "  FAIL [%s:%d] %s: expected %lld, got %lld\n",   \
                    __FILE__, __LINE__, msg,                                   \
                    (long long)(b), (long long)(a));                          \
            g_fails++;                                                        \
        }                                                                     \
    } while (0)

struct Timer {
    std::chrono::high_resolution_clock::time_point start_;
    Timer() { reset(); }
    void reset() { start_ = std::chrono::high_resolution_clock::now(); }
    int64_t elapsed_ms() const {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   end - start_).count();
    }
};

// ── Helpers ────────────────────────────────────────────────────────
static void fill_pattern(void *p, size_t sz, uint8_t seed) {
    auto *buf = static_cast<uint8_t *>(p);
    for (size_t i = 0; i < sz; i++)
        buf[i] = static_cast<uint8_t>(seed + i);
}
static bool check_pattern(const void *p, size_t sz, uint8_t seed) {
    auto *buf = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < sz; i++) {
        if (buf[i] != static_cast<uint8_t>(seed + i))
            return false;
    }
    return true;
}

// ── Individual tests ───────────────────────────────────────────────

static int test_init() {
    printf("  init: tm_region_init + tm_region_destroy\n");

    int rc = stm::tm_region_init();
    TEQ(rc, 0, "tm_region_init returns 0");

    T(stm::g_tm_region_start != nullptr, "g_tm_region_start non-null");
    T(stm::g_tm_region_end != nullptr,   "g_tm_region_end non-null");
    T(stm::g_tm_region_end > stm::g_tm_region_start,
      "end > start");
    T(stm::g_slab_size > 0,   "g_slab_size > 0");
    T(stm::g_num_slabs > 0,   "g_num_slabs > 0");

    // Double init safe
    int rc2 = stm::tm_region_init();
    TEQ(rc2, 0, "double init returns 0");

    stm::tm_region_destroy();
    return g_fails;
}

static int test_malloc_small() {
    printf("  malloc_small: tm_region_malloc(1…256)\n");
    int local = g_fails;
    stm::tm_region_init();

    std::set<void *> seen;
    for (size_t sz = 1; sz <= 256; sz++) {
        void *p = stm::tm_region_malloc(sz);
        T(p != nullptr,             "small malloc non-null");
        T(seen.count(p) == 0,       "small malloc distinct");
        seen.insert(p);
        fill_pattern(p, sz, static_cast<uint8_t>(sz));
        T(check_pattern(p, sz, static_cast<uint8_t>(sz)),
          "small malloc write/verify");
    }

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_malloc_large() {
    printf("  malloc_large: tm_region_malloc(1KB…1MB)\n");
    int local = g_fails;
    stm::tm_region_init();

    for (size_t sz = 1024; sz <= 1024 * 1024; sz *= 4) {
        void *p = stm::tm_region_malloc(sz);
        T(p != nullptr, "large malloc non-null");
        fill_pattern(p, sz, 0xAB);
        T(check_pattern(p, sz, 0xAB), "large malloc write/verify");
    }

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_alignment() {
    printf("  alignment: every allocation is 16-byte aligned\n");
    int local = g_fails;
    stm::tm_region_init();

    for (size_t sz = 1; sz <= 128; sz++) {
        void *p = stm::tm_region_malloc(sz);
        T((reinterpret_cast<uintptr_t>(p) & 15) == 0,
          "16-byte alignment");
    }
    for (size_t sz = 1024; sz <= 64 * 1024; sz *= 2) {
        void *p = stm::tm_region_malloc(sz);
        T((reinterpret_cast<uintptr_t>(p) & 15) == 0,
          "16-byte alignment (large)");
    }
    // Edge cases: odd sizes
    for (size_t sz : {17, 33, 65, 1, 3, 7, 9, 31, 63}) {
        void *p = stm::tm_region_malloc(sz);
        T((reinterpret_cast<uintptr_t>(p) & 15) == 0,
          "16-byte alignment (odd size)");
    }

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_isTMAddress() {
    printf("  isTMAddress: region membership detection\n");
    int local = g_fails;
    stm::tm_region_init();

    const char *start = stm::g_tm_region_start;
    const char *end   = stm::g_tm_region_end;

    T(stm::isTMAddress(start),          "start is in region");
    T(stm::isTMAddress(end - 1),        "end-1 is in region");
    T(stm::isTMAddress(start + 1),      "start+1 is in region");
    T(stm::isTMAddress(start + 4096),   "start+4KB is in region");

    void *p1 = stm::tm_region_malloc(64);
    void *p2 = stm::tm_region_malloc(64);
    T(stm::isTMAddress(p1), "allocated ptr p1 is in region");
    T(stm::isTMAddress(p2), "allocated ptr p2 is in region");

    // Below start
    uintptr_t below = reinterpret_cast<uintptr_t>(start) - 1;
    T(!stm::isTMAddress(reinterpret_cast<void *>(below)),
      "below start not in region");

    // Above end
    uintptr_t above = reinterpret_cast<uintptr_t>(end);
    T(!stm::isTMAddress(reinterpret_cast<void *>(above)),
      "address at end not in region");
    above = reinterpret_cast<uintptr_t>(end) + 1;
    T(!stm::isTMAddress(reinterpret_cast<void *>(above)),
      "address above end not in region");

    T(!stm::isTMAddress(nullptr), "null not in region");

    int stack_var = 0;
    T(!stm::isTMAddress(&stack_var), "stack addr not in region");

    void *heap_p = std::malloc(64);
    T(!stm::isTMAddress(heap_p), "heap addr not in region");
    std::free(heap_p);

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_calloc() {
    printf("  calloc: tm_region_calloc zeroes memory\n");
    int local = g_fails;
    stm::tm_region_init();

    void *p = stm::tm_region_calloc(1, 128);
    T(p != nullptr, "calloc non-null");
    auto *buf = static_cast<uint8_t *>(p);
    for (int i = 0; i < 128; i++)
        T(buf[i] == 0, "calloc byte is zero");

    p = stm::tm_region_calloc(128, 1);
    T(p != nullptr, "calloc(128,1) non-null");
    buf = static_cast<uint8_t *>(p);
    for (int i = 0; i < 128; i++)
        T(buf[i] == 0, "calloc(128,1) byte zero");

    // Zero-count edge cases
    p = stm::tm_region_calloc(0, 1024);
    T(p != nullptr, "calloc(0,1024) non-null");
    p = stm::tm_region_calloc(1024, 0);
    T(p != nullptr, "calloc(1024,0) non-null");

    // Large calloc — mmap pages are zero-initialised
    size_t large = 4 * 1024 * 1024;
    p = stm::tm_region_calloc(large, 1);
    T(p != nullptr, "calloc(4MB) non-null");
    buf = static_cast<uint8_t *>(p);
    for (size_t i = 0; i < large; i += 4096)
        T(buf[i] == 0, "calloc(4MB) page-start zero");

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_realloc() {
    printf("  realloc: tm_region_realloc semantics\n");
    int local = g_fails;
    stm::tm_region_init();

    // realloc(null, sz) ≡ malloc(sz)
    void *p = stm::tm_region_realloc(nullptr, 64);
    T(p != nullptr, "realloc(NULL,64) non-null");

    // realloc(p, 0) ≡ free(p) → returns nullptr
    void *r = stm::tm_region_realloc(p, 0);
    (void)r;

    // realloc(ptr, larger) — new block, old dropped
    p = stm::tm_region_malloc(16);
    fill_pattern(p, 16, 0x42);
    void *q = stm::tm_region_realloc(p, 256);
    T(q != nullptr, "realloc grow non-null");
    T(q != p,       "realloc grow different ptr (bump)");
    fill_pattern(q, 256, 0x42);
    T(check_pattern(q, 256, 0x42), "realloc grow write/verify");

    // realloc(ptr, smaller) — new block
    void *r2 = stm::tm_region_realloc(q, 8);
    T(r2 != nullptr, "realloc shrink non-null");
    T(r2 != q,       "realloc shrink different ptr");
    fill_pattern(r2, 8, 0x99);
    T(check_pattern(r2, 8, 0x99), "realloc shrink write/verify");

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_multi_thread() {
    int nthreads = (int)std::thread::hardware_concurrency();
    printf("  multi_thread: concurrent allocation from %d threads\n",
           nthreads);
    int local = g_fails;
    stm::tm_region_init();

    const int ALLOCS_PER = 1000;
    std::vector<std::thread> threads;
    std::vector<std::vector<void *>> ptrs(nthreads);

    for (int t = 0; t < nthreads; t++) {
        threads.emplace_back([t, &ptrs, nthreads]() {
            std::vector<void *> local_ptrs;
            for (int i = 0; i < ALLOCS_PER; i++) {
                size_t sz = (i % 8 + 1) * 16;
                void *p = stm::tm_region_malloc(sz);
                if (!p) {
                    fprintf(stderr,
                            "  FAIL thread %d: null at iter %d\n",
                            t, i);
                    return;
                }
                fill_pattern(p, sz,
                             static_cast<uint8_t>(t * 64 + i));
                local_ptrs.push_back(p);
            }
            ptrs[t] = std::move(local_ptrs);
        });
    }
    for (auto &th : threads) th.join();

    // Verify every thread's allocations
    for (int t = 0; t < nthreads; t++) {
        for (size_t i = 0; i < ptrs[t].size(); i++) {
            size_t sz = (i % 8 + 1) * 16;
            void *p = ptrs[t][i];
            T(stm::isTMAddress(p), "multi-thread ptr in region");
            T(check_pattern(p, sz,
                           static_cast<uint8_t>(t * 64 + i)),
              "multi-thread data intact");
        }
    }

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_oversized() {
    printf("  oversized: allocation larger than g_slab_size\n");
    int local = g_fails;
    stm::tm_region_init();

    size_t slab_sz = stm::g_slab_size;
    T(slab_sz > 0, "slab size positive");

    // 2× slab size + 1 → claims 3 consecutive slabs
    size_t big = slab_sz * 2 + 1;
    void *p = stm::tm_region_malloc(big);
    T(p != nullptr,           "oversized malloc non-null");
    T(stm::isTMAddress(p),    "oversized ptr in region");
    fill_pattern(p, big, 0xFF);
    T(check_pattern(p, big, 0xFF), "oversized write/verify");

    // 3× slab size
    size_t huge = slab_sz * 3;
    void *q = stm::tm_region_malloc(huge);
    T(q != nullptr,           "huge malloc non-null");
    fill_pattern(q, huge, 0xFE);
    T(check_pattern(q, huge, 0xFE), "huge write/verify");

    // No overlap
    auto *p_start = static_cast<const char *>(p);
    auto *p_end   = p_start + big;
    auto *q_start = static_cast<const char *>(q);
    T(q_start < p_start || q_start >= p_end,
      "oversized allocs do not overlap");

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_bump_order() {
    printf("  bump_order: sequential allocations non-overlapping\n");
    int local = g_fails;
    stm::tm_region_init();

    const int N = 256;
    std::vector<void *> ptrs;
    std::vector<size_t> sizes;

    for (int i = 0; i < N; i++) {
        size_t sz = (i % 32 + 1) * 7;
        void *p = stm::tm_region_malloc(sz);
        T(p != nullptr, "bump-order alloc non-null");
        fill_pattern(p, sz, static_cast<uint8_t>(i));
        ptrs.push_back(p);
        sizes.push_back(sz);
    }

    // Sort by address and verify no overlap
    using Range = std::pair<char *, char *>;
    std::vector<Range> ranges;
    for (size_t i = 0; i < ptrs.size(); i++) {
        auto *start = static_cast<char *>(ptrs[i]);
        size_t aligned = (sizes[i] + 15) & ~15ULL;
        ranges.emplace_back(start, start + aligned);
    }
    std::sort(ranges.begin(), ranges.end(),
              [](auto &a, auto &b) { return a.first < b.first; });
    for (size_t i = 1; i < ranges.size(); i++)
        T(ranges[i].first >= ranges[i - 1].second,
          "sequential allocs do not overlap");

    // Verify data
    for (int i = 0; i < N; i++) {
        size_t sz = (i % 32 + 1) * 7;
        T(check_pattern(ptrs[i], sz, static_cast<uint8_t>(i)),
          "bump-order data intact");
    }

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_zero_size() {
    printf("  zero_size: tm_region_malloc(0) returns non-null\n");
    int local = g_fails;
    stm::tm_region_init();

    void *p = stm::tm_region_malloc(0);
    T(p != nullptr,         "malloc(0) non-null");
    T(stm::isTMAddress(p),  "malloc(0) in region");

    void *q = stm::tm_region_malloc(0);
    T(q != nullptr,          "second malloc(0) non-null");
    T(q != p,                "second malloc(0) distinct");

    stm::tm_region_destroy();
    return g_fails - local;
}

static int test_slab_exhaustion() {
    printf("  slab_exhaustion: single thread consumes multiple slabs\n");
    int local = g_fails;
    stm::tm_region_init();

    size_t slab_sz = stm::g_slab_size;
    size_t per_alloc = slab_sz / 8;
    int total = (int)(3 * slab_sz / per_alloc) + 10;

    for (int i = 0; i < total; i++) {
        void *p = stm::tm_region_malloc(per_alloc);
        T(p != nullptr, "slab-exhaustion alloc non-null");
        fill_pattern(p, per_alloc, static_cast<uint8_t>(i));
    }

    // Still works after exhaustion
    void *p = stm::tm_region_malloc(16);
    T(p != nullptr, "post-exhaustion alloc works");
    fill_pattern(p, 16, 0x77);
    T(check_pattern(p, 16, 0x77), "post-exhaustion data intact");

    stm::tm_region_destroy();
    return g_fails - local;
}

// ── Main ───────────────────────────────────────────────────────────

int main() {
    printf("\n═══ TM Region Allocator Unit Tests ═══\n\n");

    struct TestCase {
        const char *name;
        int (*fn)();
    };
    TestCase cases[] = {
        {"init",             test_init},
        {"malloc_small",     test_malloc_small},
        {"malloc_large",     test_malloc_large},
        {"alignment",        test_alignment},
        {"isTMAddress",      test_isTMAddress},
        {"calloc",           test_calloc},
        {"realloc",          test_realloc},
        {"multi_thread",     test_multi_thread},
        {"oversized",        test_oversized},
        {"bump_order",       test_bump_order},
        {"zero_size",        test_zero_size},
        {"slab_exhaustion",  test_slab_exhaustion},
    };

    int failures = 0;
    for (auto &c : cases) {
        printf("%s:\n", c.name);
        int f = c.fn();
        printf("  → %s (%d failures)\n", f == 0 ? "PASS" : "FAIL", f);
        failures += f;
    }

    printf("\n═══ %s (total failures: %d) ═══\n\n",
           failures == 0 ? "ALL PASS" : "SOME FAILED", failures);
    return failures > 0 ? 1 : 0;
}
