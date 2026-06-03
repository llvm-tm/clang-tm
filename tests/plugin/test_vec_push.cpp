// Minimal reproducer: just vector push_back in a TX.
// Tests whether the plugin's instrumentation of std::vector::push_back is correct.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

TM std::vector<int64_t> g_vec;
TM std::atomic<int64_t> g_vec_total{0};
TM std::atomic<int64_t> g_vec_pushes{0};

std::atomic<bool> g_start{false};
std::atomic<bool> g_stop{false};

const int64_t ITEMS_PER_TX = 10;
const int64_t TX_PER_WORKER = 100;

TX void push_tx(int64_t base)
{
    for (int64_t i = 0; i < ITEMS_PER_TX; i++) {
        g_vec.push_back(base + i);
    }
    g_vec_pushes.fetch_add(ITEMS_PER_TX);
}

THREAD void worker(int id)
{
    std::mt19937 rng((unsigned)(id * 12345 + 1));
    while (!g_start.load())
        std::this_thread::yield();
    for (int iter = 0; iter < TX_PER_WORKER; iter++) {
        int64_t base = (int64_t)rng() % 1000000;
        push_tx(base);
    }
}

MAIN int main(int argc, char *argv[])
{
    int n_threads = 2;
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "-j") == 0)
            n_threads = atoi(argv[++i]);
    }

    printf("Vector push_back TX test\n");
    printf("  threads: %d\n", n_threads);
    printf("  TX/worker: %d\n\n", TX_PER_WORKER);

    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; i++)
        threads.emplace_back(worker, i);

    g_start.store(true);
    for (auto &t : threads)
        t.join();
    g_stop.store(true);

    printf("  g_vec.size() = %zu\n", g_vec.size());
    printf("  pushes = %lld\n", (long long)g_vec_pushes.load());
    bool ok = g_vec.size() <= g_vec_pushes.load();
    printf("\n  Result: %s\n", ok ? "PASS" : "FAIL (push count mismatch)");
    return ok ? 0 : 1;
}
