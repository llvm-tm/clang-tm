// test_phase_switch.cpp
// Tests tm_swap_runtime() for phase-based TM: runs transfers under stubs
// (single-thread direct access), then "stop the world" via a global barrier,
// swaps to real TinySTM hooks, and continues with multi-threaded TM.
// Verifies money conservation across both phases.

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "tm_api.hpp"
#include "backends/tm_impl/common/tm_hooks.hpp"

// ── Bank parameters ──────────────────────────────────────────────
static constexpr int NUM_ACCOUNTS = 32;
static constexpr int64_t INIT_BALANCE = 1000;
static constexpr int64_t EXPECTED = NUM_ACCOUNTS * INIT_BALANCE;

// Heap-backed accounts (Phase 1: stubs) and TM-region accounts (Phase 2)
static int64_t *g_heap_accts = nullptr;
static int64_t *g_tm_accts   = nullptr;

static int64_t total(int64_t *a) {
    int64_t s = 0;
    for (int i = 0; i < NUM_ACCOUNTS; i++) s += a[i];
    return s;
}

static void transfer(int64_t *a, int src, int dst, int64_t amt) {
    int64_t v = tm_read_i8((uint64_t*)&a[src]);
    if (v >= amt) {
        tm_write_i8((uint64_t*)&a[src], (uint64_t)(v - amt));
        v = tm_read_i8((uint64_t*)&a[dst]);
        tm_write_i8((uint64_t*)&a[dst], (uint64_t)(v + amt));
    }
}

// ── Phase 1 worker (stubs, single thread) ────────────────────────
static std::atomic<int64_t> g_phase1_ops{0};
static std::atomic<bool> g_phase1_done{false};

static void worker_stubs() {
    std::mt19937 rng(100);
    std::uniform_int_distribution<int> acct(0, NUM_ACCOUNTS - 1);
    std::uniform_int_distribution<int64_t> amt(1, 100);

    int64_t ops = 0;
    while (!g_phase1_done.load()) {
        int s = acct(rng), d = acct(rng);
        if (s == d) continue;
        tm_begin();
        transfer(g_heap_accts, s, d, amt(rng));
        tm_end();
        ops++;
    }
    g_phase1_ops.fetch_add(ops);
}

// ── Phase 2 worker (real TM) ─────────────────────────────────────
static std::atomic<int64_t> g_phase2_ops{0};
static std::atomic<bool> g_phase2_done{false};

static void worker_tm(int id) {
    tm_init_thread();

    std::mt19937 rng(id + 200);
    std::uniform_int_distribution<int> acct(0, NUM_ACCOUNTS - 1);
    std::uniform_int_distribution<int64_t> amt(1, 100);

    int64_t ops = 0;
    while (!g_phase2_done.load()) {
        int s = acct(rng), d = acct(rng);
        if (s == d) continue;

        volatile bool committed = false;
        while (!committed) {
            sigsetjmp(tm_jmpbuf, 0);
            tm_nested_call_counter = 1;
            tm_begin();
            transfer(g_tm_accts, s, d, amt(rng));
            tm_end();
            committed = true;
        }
        ops++;
    }
    g_phase2_ops.fetch_add(ops);

    tm_exit_thread();
}

// ── Main ─────────────────────────────────────────────────────────
int main() {
    printf("=== Phase-Switch TM Test ===\n");
    printf("Accounts: %d  Balance: %lld  Expected: %lld\n",
           NUM_ACCOUNTS, (long long)INIT_BALANCE, (long long)EXPECTED);

    // Init TM: registers TinySTM hooks but thread count stays 1 → stubs active
    tm_init();

    // ── Phase 1: stubs (single-thread, heap accounts) ──────────
    g_heap_accts = (int64_t*)std::malloc(NUM_ACCOUNTS * sizeof(int64_t));
    for (int i = 0; i < NUM_ACCOUNTS; i++) g_heap_accts[i] = INIT_BALANCE;
    printf("Initial total: %lld\n\n", (long long)total(g_heap_accts));

    printf("Phase 1: stubs (single-thread, direct access on heap)\n");
    g_phase1_done.store(false);
    std::thread p1(worker_stubs);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    g_phase1_done.store(true);
    p1.join();
    printf("  Ops: %lld  Total: %lld  %s\n",
           (long long)g_phase1_ops.load(),
           (long long)total(g_heap_accts),
           total(g_heap_accts) == EXPECTED ? "PASS" : "FAIL");

    // ── Stop the world: allocate in TM region, copy, swap hooks ─
    printf("\nStop the world: tm_swap_runtime -> TinySTM\n");

    // Swap to real TinySTM hooks
    const TMRealHooks *real = tm_get_real_hooks();
    if (!real || !real->begin) {
        printf("ERROR: no real hooks registered\n");
        return 1;
    }
    tm_swap_runtime(real);

    // Now that tm_malloc points to real_tm_malloc (TM region),
    // allocate TM-region accounts and copy Phase 1 state into them
    g_tm_accts = (int64_t*)tm_malloc(NUM_ACCOUNTS * sizeof(int64_t));
    std::memcpy(g_tm_accts, g_heap_accts, NUM_ACCOUNTS * sizeof(int64_t));
    printf("Copied %lld to TM region\n", (long long)total(g_tm_accts));

    // ── Phase 2: real TM (multi-threaded, TM-region accounts) ───
    printf("Phase 2: real TM hooks (4 threads)\n");
    const int N = 4;
    g_phase2_done.store(false);
    std::vector<std::thread> p2;
    for (int i = 0; i < N; i++)
        p2.emplace_back(worker_tm, i + N);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g_phase2_done.store(true);
    for (auto &t : p2) t.join();
    printf("  Ops: %lld  Total: %lld  %s\n",
           (long long)g_phase2_ops.load(),
           (long long)total(g_tm_accts),
           total(g_tm_accts) == EXPECTED ? "PASS" : "FAIL");

    // ── Done ────────────────────────────────────────────────────
    printf("\nOverall: %s\n", total(g_tm_accts) == EXPECTED ? "PASS" : "FAIL");
    tm_exit();
    return total(g_tm_accts) == EXPECTED ? 0 : 1;
}
