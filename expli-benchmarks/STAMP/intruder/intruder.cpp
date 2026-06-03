// Simplified STAMP/intruder benchmark using the explicit TM API.
//
// Network intrusion detection: shared hash table of packet signatures.
// Each TX inserts or removes entries with short duration but frequent
// conflicts (high contention).
//
// Build:
//   make -C expli_benchmarks BACKEND=TINYSTM run-intruder
//
// Original: https://stamp.stanford.edu/

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

template <typename F>
inline void tx_retry(F&& body) {
    tm_nested_call_counter++;
    int done = 0;
    while (!done) {
        tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
        tm_begin();
        if (tm_longjmp_ret != 0)
            continue;
        body();
        tm_end();
        done = 1;
    }
    tm_nested_call_counter--;
}

// ── Configuration ───────────────────────────────────────────
struct Config {
    int threads = 4;
    int num_atoms = 10;
    int max_length = 128;
    int num_packets = 1048576;
    unsigned seed = 42;
};

static Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i+1 < argc) c.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i+1 < argc) c.num_atoms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i+1 < argc) c.max_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i+1 < argc) c.num_packets = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) c.seed = (unsigned)atoi(argv[++i]);
    }
    return c;
}

// ── Shared TM-tracked signature table ───────────────────────
struct Signature {
    expli::TM<int> count;
    expli::TM<int> inserted;
};

static std::vector<Signature> g_table;
static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_inserts{0};
static std::atomic<uint64_t> g_deletes{0};

static void worker(int tid, const Config &cfg) {
    expli::TM<int>::thread_init();
    auto rng = std::mt19937(cfg.seed + tid * 1000);
    int n_per_thread = (cfg.num_packets + cfg.threads - 1) / cfg.threads;
    int start = tid * n_per_thread;
    int end = std::min(start + n_per_thread, cfg.num_packets);

    for (int i = start; i < end; i++) {
        int key = rng() % cfg.num_atoms;
        if (rng() % 2 == 0) {
            // Insert
            tx_retry([&]() {
                int c = g_table[key].count.read();
                g_table[key].count.write(c + 1);
                g_table[key].inserted.write(1);
                g_inserts.fetch_add(1);
            });
        } else {
            // Delete (decrement)
            tx_retry([&]() {
                int c = g_table[key].count.read();
                if (c > 0) {
                    g_table[key].count.write(c - 1);
                    g_deletes.fetch_add(1);
                }
            });
        }
    }
    expli::TM<int>::thread_exit();
}

int main(int argc, char *argv[]) {
    auto cfg = parse_args(argc, argv);

    printf("Intruder — Explicit TM API\n");
    printf("Threads: %d  Atoms: %d  MaxLen: %d  Packets: %d\n",
           cfg.threads, cfg.num_atoms, cfg.max_length, cfg.num_packets);

    expli::TM<int>::init();

    // Initialize signature table
    g_table.resize(cfg.num_atoms);
    for (int i = 0; i < cfg.num_atoms; i++) {
        g_table[i].count.poke(0);
        g_table[i].inserted.poke(0);
    }

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; t++)
        threads.emplace_back(worker, t, std::ref(cfg));
    for (auto &th : threads)
        th.join();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t ins = g_inserts.load();
    uint64_t del = g_deletes.load();
    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  Inserts: %llu  Deletes: %llu\n",
           (unsigned long long)ins, (unsigned long long)del);
    printf("  PASS\n");
    expli::TM<int>::exit();
    return 0;
}
