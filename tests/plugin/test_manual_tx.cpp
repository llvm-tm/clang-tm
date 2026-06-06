// Minimal manually-instrumented TM test.
// Compiled WITHOUT the plugin to verify TM runtime works correctly.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <thread>
#include <vector>

// tinystm_wbctl.hpp includes tinystm_common.hpp which has all declarations.
// globals are defined in TinySTM_runtime.cpp (not included here to avoid ODR).
#include "tinystm_wbctl.hpp"

// Manual TM wrappers using tinystm namespace
using tinystm::tm_read_i8;
using tinystm::tm_write_i8;
using tinystm::begin;
using tinystm::commit;
using tinystm::abort_tx;
using tinystm::init_thread;
using tinystm::init;

// ---- Shared data ----
static int64_t *g_buf = nullptr;
static size_t g_buf_size = 0;
static std::atomic<int64_t> g_total{0};

static const int ITERS = 1000;

// ---- Manual TX helpers ----

__thread sigjmp_buf g_my_jmpbuf;

static void tx_begin_manual() {
    tinystm::jmpbuf = &g_my_jmpbuf;
    if (sigsetjmp(g_my_jmpbuf, 0) != 0) {
        tinystm::begin();
        return;
    }
    tinystm::begin();
}

static bool tx_end_manual() {
    return tinystm::commit();
}

// ---- TX function: increment array elements ----

void array_tx_worker(int id) {
    tinystm::init_thread();
    std::mt19937 rng((unsigned)(id * 12345 + 1));

    for (int iter = 0; iter < ITERS; iter++) {
        tx_begin_manual();

        int64_t idx = (int64_t)(rng() % g_buf_size);
        int64_t old = tm_read_i8((uint64_t*)&g_buf[idx]);
        int64_t newv = old + 1;
        tm_write_i8((uint64_t*)&g_buf[idx], newv);

        if (!tx_end_manual()) {
            fprintf(stderr, "  [WORKER %d] TX ABORTED (iter %d)!\n", id, iter);
        }
    }
}

int main(int argc, char *argv[]) {
    tinystm::init();

    int n_workers = 2;
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "-j") == 0)
            n_workers = atoi(argv[++i]);
    }

    g_buf_size = 64;
    g_buf = new int64_t[g_buf_size]();

    printf("Manual TM Test (array increment)\n");
    printf("  workers: %d\n", n_workers);
    printf("  iters:   %d\n\n", ITERS);

    std::vector<std::thread> threads;
    for (int i = 0; i < n_workers; i++)
        threads.emplace_back(array_tx_worker, i);

    for (auto &t : threads)
        t.join();

    int64_t expected = (int64_t)(n_workers * ITERS);
    int64_t actual = 0;
    for (size_t i = 0; i < g_buf_size; i++)
        actual += g_buf[i];

    printf("Expected total: %lld, Actual: %lld\n",
           (long long)expected, (long long)actual);
    if (actual == expected) {
        printf("\n  RESULT: PASS\n");
        return 0;
    } else {
        printf("\n  RESULT: FAIL (lost updates: %lld)\n",
               (long long)(expected - actual));
        return 1;
    }
}
