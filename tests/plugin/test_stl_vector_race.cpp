/**
 * STL std::vector Race Reproducer
 *
 * Demonstrates why std::vector CANNOT be used as a shared TM-tracked
 * container inside transactions.  Models the exact pattern that caused
 * the yada work-heap data race:
 *
 *   struct TM { std::vector<int> heap; };
 *   TX void pop() { int v = heap.back(); heap.pop_back(); ... }
 *   TX void push(int v) { heap.push_back(v); }
 *
 * The LLVM TM pass instruments writes to the vector's internal fields
 * (pointer, size, capacity) but does NOT instrument writes to the data
 * stored through those internal pointers.  When concurrent threads read
 * and write the same vector elements from different TX functions, the
 * TM runtime cannot detect the conflict, leading to data corruption.
 *
 * Compare with:
 *   test_simple_vector.cpp — custom SimpleVec with TM-tracked fields
 *   test_vector_realloc.cpp — shared vector with concurrent read/write
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

// ── BUGGY: shared std::vector in TM struct ────────────────────────────────
// Writes to the vector DATA (not its internal pointers) are NOT tracked
// by the TM runtime.  Concurrent push/pop without TM visibility causes
// silent data corruption, crashes, or infinite loops.
struct TM SharedWorkHeap {
    std::vector<int> heap;
};

TM SharedWorkHeap g_buggy;
TM std::atomic<int64_t> g_ops_done{0};
TM std::atomic<bool> g_start{false};
TM std::atomic<bool> g_stop{false};

const int PUSHES_PER_TX = 100;
const int TXS_PER_THREAD = 20;

TX void buggy_push(int base) {
    for (int i = 0; i < PUSHES_PER_TX; i++) {
        g_buggy.heap.push_back(base + i);
    }
}

TX int buggy_pop() {
    if (g_buggy.heap.empty()) return -1;
    int val = g_buggy.heap.back();
    g_buggy.heap.pop_back();
    return val;
}

TX void buggy_read_all() {
    volatile int64_t sum = 0;
    for (size_t i = 0; i < g_buggy.heap.size(); i++) {
        sum += g_buggy.heap[i];
    }
    (void)sum;
}

THREAD void buggy_writer(int tid) {
    while (!g_start.load()) std::this_thread::yield();
    for (int t = 0; t < TXS_PER_THREAD; t++) {
        buggy_push(tid * TXS_PER_THREAD * PUSHES_PER_TX + t * PUSHES_PER_TX);
        g_ops_done.fetch_add(PUSHES_PER_TX);
    }
}

THREAD void buggy_reader() {
    while (!g_start.load()) std::this_thread::yield();
    while (!g_stop.load()) {
        buggy_read_all();
    }
}

// ── FIXED: flat array with tm_calloc + explicit read/write macros ─────────
// The fix used in yada_bench.hpp: bypass std::vector entirely, use flat
// arrays allocated via tm_calloc, and access elements through macros that
// the LLVM pass sees as plain loads/stores on TM-tracked base pointers.

#define TM_READ(p)     (*(const int*)(p))
#define TM_WRITE(p, v) (*(int*)(p) = (int)(v))

struct TM FixedWorkHeap {
    int*  data;        // tm_calloc'd array
    long* count;       // tm_calloc'd counter
};

TM FixedWorkHeap g_fixed;

extern "C" void* tm_calloc(size_t nmemb, size_t size);

static void init_fixed() {
    g_fixed.data  = (int*)tm_calloc(100000, sizeof(int));
    g_fixed.count = (long*)tm_calloc(1, sizeof(long));
}

TX void fixed_push(int val) {
    long n = TM_READ(g_fixed.count);
    TM_WRITE(&g_fixed.data[n], val);
    TM_WRITE(g_fixed.count, n + 1);
}

TX int fixed_pop() {
    long n = TM_READ(g_fixed.count);
    if (n == 0) return -1;
    long last = n - 1;
    int val = TM_READ(&g_fixed.data[last]);
    TM_WRITE(g_fixed.count, last);
    return val;
}

TX void fixed_read_all() {
    long n = TM_READ(g_fixed.count);
    volatile int64_t sum = 0;
    for (long i = 0; i < n; i++) {
        sum += TM_READ(&g_fixed.data[i]);
    }
    (void)sum;
}

THREAD void fixed_writer(int tid) {
    while (!g_start.load()) std::this_thread::yield();
    for (int t = 0; t < TXS_PER_THREAD; t++) {
        for (int i = 0; i < PUSHES_PER_TX; i++) {
            fixed_push(tid * TXS_PER_THREAD * PUSHES_PER_TX + t * PUSHES_PER_TX + i);
        }
        g_ops_done.fetch_add(PUSHES_PER_TX);
    }
}

THREAD void fixed_reader() {
    while (!g_start.load()) std::this_thread::yield();
    while (!g_stop.load()) {
        fixed_read_all();
    }
}

// ── Main ──────────────────────────────────────────────────────────────────

MAIN int main() {
    printf("STL std::vector Race Reproducer\n");
    printf("================================\n\n");

    // ── Buggy test: shared std::vector, 2 writers + 1 reader ──────────
    printf("Phase 1: BUGGY shared std::vector (2 writers + 1 reader)\n");
    printf("  Expectation: data corruption, crash, or hang\n\n");

    g_start.store(false);
    g_stop.store(false);
    g_ops_done.store(0);

    std::thread w1(buggy_writer, 0);
    std::thread w2(buggy_writer, 1);
    std::thread r(buggy_reader);

    g_start.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    g_stop.store(true);

    w1.join();
    w2.join();
    r.join();

    int64_t expected = 2 * TXS_PER_THREAD * PUSHES_PER_TX;
    int64_t actual = (int64_t)g_buggy.heap.size();
    bool buggy_ok = (actual == expected);

    if (buggy_ok) {
        printf("  Phase 1: vector.size() = %lld (== %lld) — LUCKY, no visible corruption\n",
               (long long)actual, (long long)expected);
        printf("  (but may still have silent data corruption in element values)\n");
    } else {
        printf("  Phase 1: vector.size() = %lld (expected %lld) — DATA CORRUPTION\n",
               (long long)actual, (long long)expected);
    }

    // ── Fixed test: tm_calloc flat array, 2 writers + 1 reader ────────
    printf("\nPhase 2: FIXED tm_calloc flat array (2 writers + 1 reader)\n");
    printf("  Expectation: clean run, no corruption\n\n");

    init_fixed();
    g_start.store(false);
    g_stop.store(false);
    g_ops_done.store(0);

    std::thread fw1(fixed_writer, 0);
    std::thread fw2(fixed_writer, 1);
    std::thread fr(fixed_reader);

    g_start.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    g_stop.store(true);

    fw1.join();
    fw2.join();
    fr.join();

    long fixed_n = g_fixed.count ? *g_fixed.count : -1;
    bool fixed_ok = (fixed_n == expected);

    if (fixed_ok) {
        printf("  Phase 2: fixed.count = %ld (== %lld) — PASS\n",
               (long)fixed_n, (long long)expected);
    } else {
        printf("  Phase 2: fixed.count = %ld (expected %lld) — FAIL\n",
               (long)fixed_n, (long long)expected);
    }

    // ── Result ────────────────────────────────────────────────────────
    printf("\n=============================================\n");
    if (buggy_ok && fixed_ok) {
        printf("RESULT: Buggy phase happened to survive (no visible crash)\n");
        printf("  But the shared vector ops are NOT truly TM-safe.\n");
        printf("  Run with more threads/iterations to trigger failure.\n\n");
    }
    if (fixed_ok) {
        printf("PASS: tm_calloc flat array works correctly\n");
    } else {
        printf("FAIL: tm_calloc flat array also failed\n");
    }

    return fixed_ok ? 0 : 1;
}
