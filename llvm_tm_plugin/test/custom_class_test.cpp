/**
 * Custom Class Method Test for LLVM TM Plugin
 *
 * Tests that the plugin correctly instruments methods called on
 * TM-annotated objects, including heap-memory access through pointer fields.
 *
 * Scenario:
 *   class A { int *_a; public: A() { _a = new int(0); } void inc(); };
 *   TM A a;
 *   TX void foo() { a.inc(); }
 *
 * The method a.inc() should be cloned and its body instrumented so that
 * the heap access *_a = *_a + 1 uses tm_read/tm_write.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// ---- Custom class with heap-allocated pointer field ----
class A {
    int *_a;
public:
    A() { _a = new int(0); }
    ~A() { delete _a; }

    void inc() { *_a = *_a + 1; }
    int get() const { return *_a; }
    void reset() { *_a = 0; }
};

// ---- Custom class with multiple heap-allocated fields ----
class B {
    int *_x;
    int *_y;
    int  _z;      // stack-embedded field (avoids heap in struct body)
public:
    B() : _z(0) { _x = new int(0); _y = new int(0); }
    ~B() { delete _x; delete _y; }

    void add(int v) { *_x = *_x + v; }
    void mul(int v) { *_y = *_y + v; }
    int  sum() const { return *_x + *_y + _z; }
    void reset() { *_x = 0; *_y = 0; _z = 0; }
};

// ---- Global TM-annotated instances ----
TM A g_a;
TM B g_b;

// ---- Test: single field, single increment ----
TX void test_single_inc() {
    g_a.inc();
}

// ---- Test: multiple increments in a loop ----
TX void test_loop_inc(int n) {
    for (int i = 0; i < n; i++) {
        g_a.inc();
    }
}

// ---- Test: multiple heap fields via separate methods ----
TX void test_multi_field(int v1, int v2) {
    g_b.add(v1);
    g_b.mul(v2);
}

// ---- Test: read-back after write via method ----
TX void test_read_after_write() {
    g_a.inc();
    g_a.inc();
    volatile int val = g_a.get();  // read via const method
    (void)val;
}

// ============================================================================
// Worker thread for concurrent testing
// ============================================================================

std::atomic<bool> stop_workers{false};

struct WorkerData {
    int id;
    int iterations;
    std::mt19937* rng;
};

THREAD void worker(WorkerData* w) {
    std::uniform_int_distribution<int> count_dist(1, 10);

    while (!stop_workers.load() && w->iterations > 0) {
        w->iterations--;

        int n = count_dist(*w->rng);
        test_loop_inc(n);
    }
}

// ============================================================================
// Main
// ============================================================================

MAIN int main(int argc, char* argv[]) {
    int duration = 2000;
    int threads  = 2;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            duration = std::atoi(argv[++i]);
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            threads = std::atoi(argv[++i]);
    }

    std::cout << "Custom Class Method Test\n";
    std::cout << "========================\n\n";

    int errors = 0;

    // ---- Single-threaded tests ----
    std::cout << "1. Single-threaded tests\n";

    g_a.reset();
    test_single_inc();
    int val = g_a.get();
    std::cout << "   single inc: " << val;
    if (val == 1) {
        std::cout << " PASS\n";
    } else {
        std::cout << " FAIL (expected 1)\n";
        errors++;
    }

    g_a.reset();
    test_loop_inc(100);
    val = g_a.get();
    std::cout << "   loop inc(100): " << val;
    if (val == 100) {
        std::cout << " PASS\n";
    } else {
        std::cout << " FAIL (expected 100)\n";
        errors++;
    }

    g_b.reset();
    test_multi_field(10, 20);
    int s = g_b.sum();
    std::cout << "   multi field (10+20): " << s;
    if (s == 30) {
        std::cout << " PASS\n";
    } else {
        std::cout << " FAIL (expected 30)\n";
        errors++;
    }

    g_a.reset();
    test_read_after_write();
    val = g_a.get();
    std::cout << "   read after write: " << val;
    if (val == 2) {
        std::cout << " PASS\n";
    } else {
        std::cout << " FAIL (expected 2)\n";
        errors++;
    }

    // ---- Concurrent test ----
    std::cout << "\n2. Concurrent test\n";

    g_a.reset();
    stop_workers = false;

    int loops = 100;
    std::vector<WorkerData> wd(threads);
    std::vector<std::mt19937> rngs(threads);
    std::vector<std::thread> workers;

    for (int i = 0; i < threads; i++) {
        rngs[i] = std::mt19937((unsigned)(i * 12345 + 42));
        wd[i].id          = i;
        wd[i].iterations  = loops;
        wd[i].rng         = &rngs[i];
    }

    for (int i = 0; i < threads; i++)
        workers.emplace_back(worker, &wd[i]);

    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    stop_workers = true;

    for (auto& t : workers)
        t.join();

    val = g_a.get();
    std::cout << "   concurrent result: " << val << " (non-zero = PASS)\n";
    if (val > 0) {
        std::cout << "   PASS\n";
    } else {
        std::cout << "   FAIL (expected > 0)\n";
        errors++;
    }

    std::cout << "\n";
    if (errors == 0) {
        std::cout << "All tests PASSED\n";
        return 0;
    } else {
        std::cout << errors << " test(s) FAILED\n";
        return 1;
    }
}
