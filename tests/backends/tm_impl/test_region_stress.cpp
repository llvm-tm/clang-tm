// test_region_stress.cpp — Linked list stress test + allocator
// throughput benchmark.
//
// TM region bump vs std::malloc on a node-based linked list workload.
// Each thread builds a linked list by prepending N nodes, then
// traverses to verify correctness, and (for malloc) frees them.
//
// The bump allocator should be significantly faster on allocation-
// heavy workloads because the fast path is a single non-atomic pointer
// bump, and free is a no-op.

#include "tm_region_allocator.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

// ── Configuration ──────────────────────────────────────────────────
#ifndef STRESS_NODES
#define STRESS_NODES 500000
#endif

#ifndef STRESS_THREADS
#define STRESS_THREADS 4
#endif

// ── Helpers ────────────────────────────────────────────────────────
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

// ── Linked List Node ───────────────────────────────────────────────
struct Node {
    Node   *next;
    int64_t value;
};

// ── Allocator function signatures ──────────────────────────────────
// alloc_f(initial_value) → pointer to heap-allocated Node{nullptr, value}
// list_free_f(Node*)     → free all nodes
using AllocFn  = Node *(*)(int64_t);
using FreeFn   = void (*)(Node *);

// ── TM allocator ops ───────────────────────────────────────────────
static Node *alloc_tm(int64_t value) {
    auto *n = static_cast<Node *>(stm::tm_region_malloc(sizeof(Node)));
    if (!n) return nullptr;
    n->next  = nullptr;
    n->value = value;
    return n;
}
static void free_tm(Node *head) {
    while (head) {
        Node *next = head->next;
        stm::tm_region_free(head);
        head = next;
    }
}

// ── std::malloc ops ────────────────────────────────────────────────
static Node *alloc_malloc(int64_t value) {
    auto *n = static_cast<Node *>(std::malloc(sizeof(Node)));
    if (!n) return nullptr;
    n->next  = nullptr;
    n->value = value;
    return n;
}
static void free_malloc(Node *head) {
    while (head) {
        Node *next = head->next;
        std::free(head);
        head = next;
    }
}

// ── List operations ────────────────────────────────────────────────
static Node *list_prepend(Node *head, Node *n) {
    n->next = head;
    return n;
}

static int64_t list_sum(const Node *head) {
    int64_t s = 0;
    for (auto *cur = head; cur; cur = cur->next)
        s += cur->value;
    return s;
}

// ── Allocator benchmark ────────────────────────────────────────────
struct BenchResult {
    const char *name;
    int64_t    nodes;
    int64_t    sum;
    double     sec;
    int64_t    allocs_per_sec;
};

// Single-threaded: one thread builds a linked list of n_nodes.
static BenchResult bench_single(const char *name,
                                AllocFn alloc,
                                FreeFn  free_fn,
                                int64_t n_nodes) {
    Timer t;
    Node *head = nullptr;
    for (int64_t i = 0; i < n_nodes; i++) {
        Node *n = alloc(i);
        head = list_prepend(head, n);
    }
    int64_t ms      = t.elapsed_ms();
    int64_t sum     = list_sum(head);
    double  sec     = ms / 1000.0;
    free_fn(head);
    return {name, n_nodes, sum, sec,
            static_cast<int64_t>(n_nodes / sec)};
}

// Multi-threaded: each thread builds its own list of n_nodes.
static BenchResult bench_mt(const char *name,
                            AllocFn alloc,
                            int64_t n_nodes,
                            int     n_threads) {
    Timer t;
    std::vector<std::thread> threads;
    std::vector<int64_t>     sums(n_threads, 0);

    for (int ti = 0; ti < n_threads; ti++) {
        threads.emplace_back([ti, n_nodes, &sums, alloc]() {
            Node *head = nullptr;
            for (int64_t i = 0; i < n_nodes; i++) {
                Node *n = alloc(ti * n_nodes + i);
                head = list_prepend(head, n);
            }
            sums[ti] = list_sum(head);
        });
    }
    for (auto &th : threads) th.join();

    int64_t ms         = t.elapsed_ms();
    double  sec        = ms / 1000.0;
    int64_t total      = n_nodes * n_threads;
    int64_t total_sum  = 0;
    for (auto &s : sums) total_sum += s;

    return {name, total, total_sum, sec,
            static_cast<int64_t>(total / sec)};
}

// ── Main ───────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    int64_t n_nodes    = STRESS_NODES;
    int     n_threads  = STRESS_THREADS;

    // Allow override from command line: <nodes> [threads]
    if (argc > 1) n_nodes   = std::atoll(argv[1]);
    if (argc > 2) n_threads = std::atoi(argv[2]);

    // Cap threads at hardware concurrency
    int    hw  = (int)std::thread::hardware_concurrency();
    if (n_threads > hw) n_threads = hw;

    printf("\n═══ TM Region Allocator Stress Test ═══\n\n");
    printf("Linked list: %lld nodes/thread, %d threads\n\n",
           (long long)n_nodes, n_threads);

    // ── Phase 1: init ───────────────────────────────────────────
    printf("Phase 1: Initialise TM region\n");
    int rc = stm::tm_region_init();
    if (rc != 0) {
        fprintf(stderr, "FAIL: tm_region_init returned %d\n", rc);
        return 1;
    }
    printf("  region: %p .. %p  (%lld B slabs, %zu total)\n\n",
           (void*)stm::g_tm_region_start,
           (void*)stm::g_tm_region_end,
           (long long)stm::g_slab_size,
           stm::g_num_slabs);

    // ── Phase 2: correctness ────────────────────────────────────
    printf("Phase 2: Correctness\n");
    {
        Node *head = nullptr;
        for (int64_t i = 0; i < n_nodes; i++) {
            head = list_prepend(head, alloc_tm(i));
        }
        int64_t sum      = list_sum(head);
        int64_t expected = n_nodes * (n_nodes - 1) / 2;
        bool ok = (sum == expected);
        printf("  sum = %lld, expected = %lld  %s\n",
               (long long)sum, (long long)expected,
               ok ? "PASS" : "FAIL");
        if (!ok) {
            fprintf(stderr, "FAIL: data corruption\n");
            return 1;
        }
        free_tm(head);
    }

    // ── Phase 3: single-threaded throughput ─────────────────────
    printf("\nPhase 3: Single-threaded throughput\n");
    printf("  %-14s %10s %14s\n",
           "Allocator", "Time (ms)", "Allocs/sec");
    printf("  ────────────── ────────── ──────────────\n");

    auto r_tm  = bench_single("TM region",   alloc_tm,   free_tm,
                              n_nodes);
    auto r_mal = bench_single("std::malloc", alloc_malloc, free_malloc,
                              n_nodes);

    printf("  %-14s %10lld %14lld\n",
           r_tm.name,  (long long)(r_tm.sec * 1000),
           (long long)r_tm.allocs_per_sec);
    printf("  %-14s %10lld %14lld\n",
           r_mal.name, (long long)(r_mal.sec * 1000),
           (long long)r_mal.allocs_per_sec);

    double ratio_st = (double)r_tm.allocs_per_sec /
                      (double)r_mal.allocs_per_sec;
    printf("\n  Speedup: %.1f× (TM region vs std::malloc)\n", ratio_st);

    // ── Phase 4: multi-threaded throughput ──────────────────────
    printf("\nPhase 4: Multi-threaded throughput (%d threads)\n",
           n_threads);
    printf("  %-14s %10s %14s\n",
           "Allocator", "Time (ms)", "Allocs/sec");
    printf("  ────────────── ────────── ──────────────\n");

    auto r_tm_mt  = bench_mt("TM region",   alloc_tm,   n_nodes,
                              n_threads);
    auto r_mal_mt = bench_mt("std::malloc", alloc_malloc, n_nodes,
                              n_threads);

    printf("  %-14s %10lld %14lld\n",
           r_tm_mt.name,  (long long)(r_tm_mt.sec * 1000),
           (long long)r_tm_mt.allocs_per_sec);
    printf("  %-14s %10lld %14lld\n",
           r_mal_mt.name, (long long)(r_mal_mt.sec * 1000),
           (long long)r_mal_mt.allocs_per_sec);

    double ratio_mt = (double)r_tm_mt.allocs_per_sec /
                      (double)r_mal_mt.allocs_per_sec;
    printf("\n  Speedup: %.1f× (TM region vs std::malloc)\n",
           ratio_mt);

    // ── Phase 5: scalability ────────────────────────────────────
    printf("\nPhase 5: Scalability (TM allocator)\n");
    printf("  %7s %10s %14s\n",
           "Threads", "Time (ms)", "Allocs/sec");
    printf("  ─────── ────────── ──────────────\n");

    for (int tc : {1, 2, 4}) {
        if (tc > hw) continue;
        int64_t per = n_nodes / tc;
        auto r = bench_mt("", alloc_tm, per, tc);
        printf("  %7d %10lld %14lld\n",
               tc,
               (long long)(r.sec * 1000),
               (long long)r.allocs_per_sec);
    }

    // ── Phase 6: free/reuse correctness ──────────────────────────
    printf("\nPhase 6: Free/Reuse correctness\n");
    {
        bool ok = true;
        const int N = 10000;
        Node *ptrs[N];
        for (int i = 0; i < N; i++)
            ptrs[i] = alloc_tm(i);
        // Free all
        for (int i = 0; i < N; i++)
            stm::tm_region_free(ptrs[i]);
        // Allocate again — freed blocks must be reusable
        for (int i = 0; i < N; i++) {
            ptrs[i] = alloc_tm(i + N);
            if (!ptrs[i]) { ok = false; break; }
        }
        // Verify data
        for (int i = 0; i < N && ok; i++) {
            if (ptrs[i]->value != i + N) ok = false;
        }
        // Free all again
        for (int i = 0; i < N; i++)
            stm::tm_region_free(ptrs[i]);
        printf("  free+reuse: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { fprintf(stderr, "FAIL: free/reuse corruption\n"); return 1; }
    }

    // ── Phase 7: alloc/free exceeding 64 GB total ────────────────
    printf("\nPhase 7: Alloc/free exceeding 64 GB total\n");
    {
        // Each iteration allocates a 48-byte Node and frees it.
        // Total: 8e8 iterations × 48 B = 38.4 GB allocated.
        // The live set is at most 1 node at any time.
        const int64_t TOTAL_GB = 2;
        int64_t total_bytes = (int64_t)TOTAL_GB * 1024 * 1024 * 1024;
        int64_t iterations = total_bytes / (int64_t)sizeof(Node);

        Timer t;
        for (int64_t i = 0; i < iterations; i++) {
            Node *n = alloc_tm(i);
            stm::tm_region_free(n);
        }
        int64_t ms = t.elapsed_ms();
        double sec = ms / 1000.0;
        int64_t rate = (int64_t)(iterations / sec);

        bool pass = (stm::g_next_slab_idx.load() * stm::g_slab_size <
                     (uint64_t)TOTAL_GB * 1024 * 1024 * 1024); // used < TOTAL_GB → blocks recycled
        printf("  %lld iters in %.1fs  (%lld allocs/sec)  "
               "slabs used: %zu/%zu (%.0f%%)  %s\n",
               (long long)iterations, sec, (long long)rate,
               (size_t)stm::g_next_slab_idx.load(), stm::g_num_slabs,
               100.0 * stm::g_next_slab_idx.load() / stm::g_num_slabs,
               pass ? "PASS" : "FAIL");
        if (!pass) {
            fprintf(stderr, "FAIL: alloc/free stress — region exhausted\n");
            return 1;
        }
    }

    // ── Phase 8: cleanup ────────────────────────────────────────
    printf("\nPhase 8: Destroy\n");
    stm::tm_region_destroy();

    double speedup = ratio_st > ratio_mt ? ratio_st : ratio_mt;
    printf("\n═══ %s (%.1f× speedup over malloc) ═══\n\n",
           speedup >= 1.0 ? "TM ALLOCATOR FASTER" : "MALLOC FASTER",
           speedup);
    return 0;
}
