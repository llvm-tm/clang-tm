// Unit tests for TM transaction operations
// Links against a runtime backend.
#include "../tm_api.hpp"
#include <cstdio>
#include <cstdlib>

static int failures = 0;
static int tests    = 0;

#define CHECK(cond, msg) do { \
    tests++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s] %s\n", #cond, msg); \
        failures++; \
    } \
} while(0)

// ── Test basic begin/end (TM reads with TM writes) ─────────
struct Point {
    expli::TM<int> x;
    expli::TM<int> y;
};

static void test_single_transaction() {
    Point p;
    p.x.poke(0);
    p.y.poke(0);

    expli::TM<int>::begin();
    p.x.write(10);
    p.y.write(20);
    int xv = p.x.read();
    int yv = p.y.read();
    CHECK(xv == 10, "tx read x after write");
    CHECK(yv == 20, "tx read y after write");
    expli::TM<int>::end();

    // After TX, values should be committed
    CHECK(p.x.peek() == 10, "tx commit x");
    CHECK(p.y.peek() == 20, "tx commit y");
}

// ── Test TM-read sees write-set values ──────────────────────
static void test_read_own_writes() {
    expli::TM<int> val;
    val.poke(0);

    expli::TM<int>::begin();
    val.write(42);
    int r1 = val.read();
    CHECK(r1 == 42, "read own write inside tx");

    val.write(100);
    int r2 = val.read();
    CHECK(r2 == 100, "read updated own write");
    expli::TM<int>::end();

    CHECK(val.peek() == 100, "commit final value");
}

// ── Test nested TM operations ───────────────────────────────
static void test_nesting() {
    expli::TM<int> a, b;
    a.poke(1);
    b.poke(2);

    expli::TM<int>::begin();      // outer
    a.write(10);

    expli::TM<int>::begin();      // inner
    b.write(20);
    int av = a.read();
    int bv = b.read();
    CHECK(av == 10, "nested read sees outer write");
    CHECK(bv == 20, "nested read own write");
    expli::TM<int>::end();        // inner end (no-op)

    bv = b.read();
    CHECK(bv == 20, "after inner end, outer sees inner write");
    expli::TM<int>::end();        // outer end (commits both)

    CHECK(a.peek() == 10, "nesting commit a");
    CHECK(b.peek() == 20, "nesting commit b");
}

// ── Test multiple TMs of different types ────────────────────
static void test_mixed_types() {
    expli::TM<int>    i;
    expli::TM<double> d;
    expli::TM<float>  f;

    i.poke(0); d.poke(0.0); f.poke(0.0f);

    expli::TM<int>::begin();
    i.write(-99);
    d.write(3.14159);
    f.write(2.718f);

    CHECK(i.read() == -99, "mixed int");
    CHECK(d.read() > 3.14 && d.read() < 3.15, "mixed double");
    CHECK(f.read() > 2.71 && f.read() < 2.72, "mixed float");
    expli::TM<int>::end();

    CHECK(i.peek() == -99, "mixed commit int");
    CHECK(d.peek() > 3.14 && d.peek() < 3.15, "mixed commit double");
}

// ── Test TM::malloc/free inside TX ──────────────────────────
static void test_tm_malloc_free() {
    expli::TM<int>::begin();

    int *p = (int*)expli::TM<int>::malloc(100 * sizeof(int));
    CHECK(p != nullptr, "tm_malloc");

    for (int i = 0; i < 100; i++) {
        p[i] = i;
        expli::TM<int>::write_at(&p[i], p[i]);  // TM-write to track
    }

    int sum = 0;
    for (int i = 0; i < 100; i++) {
        int v = expli::TM<int>::read_at(&p[i]);  // TM-read
        sum += v;
    }
    CHECK(sum == 100*99/2, "tm_malloc read/write sum");

    expli::TM<int>::free(p);
    expli::TM<int>::end();
    // After abort/commit, freed memory is valid to observe
    printf("      (tm_malloc inside TX works — no crash)\n");
}

// ── Test TM::calloc ──────────────────────────────────────────
static void test_tm_calloc() {
    expli::TM<int>::begin();

    int *p = (int*)expli::TM<int>::calloc(50, sizeof(int));
    CHECK(p != nullptr, "tm_calloc");
    for (int i = 0; i < 50; i++) CHECK(p[i] == 0, "tm_calloc zeroed");

    expli::TM<int>::free(p);
    expli::TM<int>::end();
    printf("      (tm_calloc inside TX works — no crash)\n");
}

// ── Test TM<int*> (pointer specialization) ──────────────────
static void test_tm_pointer() {
    // Array of TM<int> — each element individually protected
    // alloc/free manage the buffer; read/write/read_at/write_at access elements
    expli::TM<int*> p;
    p.alloc(10);
    CHECK(p.peek() != nullptr, "TM<int*> alloc returns non-null");

    expli::TM<int>::begin();

    // Write elements via TM
    for (int i = 0; i < 10; i++)
        expli::TM<int>::write_at(&p.read()[i], i * 2);

    // Read back
    for (int i = 0; i < 10; i++) {
        int val = expli::TM<int>::read_at(&p.read()[i]);
        CHECK(val == i * 2, "TM<int*> element read");
    }

    expli::TM<int>::end();

    // Verify committed values
    for (int i = 0; i < 10; i++)
        CHECK(p.peek()[i] == i * 2, "TM<int*> commit check");

    p.free_ptr();
    CHECK(p.peek() == nullptr, "TM<int*> free");
}

// ── Test TM<TM<int>*> with operator[] ───────────────────────
static void test_tm_ptr_operator() {
    expli::TM< expli::TM<int> * > buf;
    buf.alloc(10);
    CHECK(buf.peek() != nullptr, "TM<TM<int>*> alloc");

    expli::TM<int>::begin();

    for (int i = 0; i < 10; i++)
        buf[i].write(i * 3);

    for (int i = 0; i < 10; i++) {
        int val = buf[i].read();
        CHECK(val == i * 3, "TM<TM<int>*> operator[] read");
    }

    expli::TM<int>::end();

    for (int i = 0; i < 10; i++)
        CHECK(buf.peek()[i].peek() == i * 3, "TM<TM<int>*> commit");

    buf.free_ptr();
}

// ── Test thread_init/thread_exit ────────────────────────────
static void test_thread_lifecycle() {
    expli::TM<int>::thread_init();

    expli::TM<int>::begin();
    expli::TM<int> v;
    v.write(77);
    int r = v.read();
    CHECK(r == 77, "tx after thread_init");
    expli::TM<int>::end();

    expli::TM<int>::thread_exit();
    printf("      (thread_init/exit OK)\n");
}

// ── Main ────────────────────────────────────────────────────
int main() {
    printf("TM transaction unit tests\n");
    printf("=========================\n\n");

    expli::TM<int>::init();
    expli::TM<int>::thread_init();

    test_single_transaction();  printf("  single TX:        %s\n", failures ? "FAIL" : "PASS");
    test_read_own_writes();     printf("  read own writes:  %s\n", failures ? "FAIL" : "PASS");
    test_nesting();             printf("  nesting:          %s\n", failures ? "FAIL" : "PASS");
    test_mixed_types();         printf("  mixed types:      %s\n", failures ? "FAIL" : "PASS");
    test_tm_malloc_free();      printf("  tm_malloc/free:   %s\n", failures ? "FAIL" : "PASS");
    test_tm_calloc();           printf("  tm_calloc:        %s\n", failures ? "FAIL" : "PASS");
    test_tm_pointer();          printf("  TM<int*> ptr:     %s\n", failures ? "FAIL" : "PASS");
    test_tm_ptr_operator();     printf("  TM<TM<int>*> []:  %s\n", failures ? "FAIL" : "PASS");
    test_thread_lifecycle();    printf("  thread lifecycle: %s\n", failures ? "FAIL" : "PASS");

    expli::TM<int>::exit();

    printf("\n");
    if (failures) {
        printf("FAILED: %d/%d tests failed\n", failures, tests);
        return 1;
    }
    printf("ALL %d tests PASSED\n", tests);
    return 0;
}
