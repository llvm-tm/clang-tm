/// Benchmark v2: compare inline vs queue for ASYNC TXes (no per-TX wait)
/// and for heavier TXes (more work per TX, amortizing overhead).

#include <cstdio>
#include <chrono>

extern "C" void tm_wait_prev_tx(void);

static int counter = 0;

// --- Sync TX (light) ---
__attribute__((noinline, annotate("shared")))
void inc_sync() {
    counter++;
}

// --- Async TX (light) ---
__attribute__((noinline, annotate("async_shared")))
void inc_async() {
    counter++;
}

// --- Heavy TX (100 increments batched) ---
__attribute__((noinline, annotate("shared")))
void inc_heavy() {
    for (int j = 0; j < 100; j++)
        counter++;
}

int main() {
    // -----------------------------------------------------------------
    // 1. Sync light — compare inline vs queue per-TX overhead
    // -----------------------------------------------------------------
    const int N_SYNC = 50000;
    counter = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N_SYNC; i++) inc_sync();
    auto end = std::chrono::high_resolution_clock::now();
    double sec1 = std::chrono::duration<double>(end - start).count();
    printf("sync-light: %d tx in %.3f s = %.0f tx/s  counter=%d\n",
           N_SYNC, sec1, N_SYNC / sec1, counter);

    // -----------------------------------------------------------------
    // 2. Async light (queue only: no per-TX wait, overlap on workers)
    // -----------------------------------------------------------------
    const int N_ASYNC = 50000;
    counter = 0;
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N_ASYNC; i++) inc_async();
    tm_wait_prev_tx(); // wait for last one
    end = std::chrono::high_resolution_clock::now();
    double sec2 = std::chrono::duration<double>(end - start).count();
    printf("async-light: %d tx in %.3f s = %.0f tx/s  counter=%d\n",
           N_ASYNC, sec2, N_ASYNC / sec2, counter);

    // -----------------------------------------------------------------
    // 3. Heavy sync (100 ops/TX — more work amortizing overhead)
    // -----------------------------------------------------------------
    const int N_HEAVY = 1000;  // 1000 * 100 = 100000 total increments
    counter = 0;
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N_HEAVY; i++) inc_heavy();
    end = std::chrono::high_resolution_clock::now();
    double sec3 = std::chrono::duration<double>(end - start).count();
    printf("heavy-sync: %d tx (%d op) in %.3f s = %.0f tx/s  counter=%d\n",
           N_HEAVY, N_HEAVY * 100, sec3, N_HEAVY / sec3, counter);

    int ok = (counter == 50000 + 50000 + 100000);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
