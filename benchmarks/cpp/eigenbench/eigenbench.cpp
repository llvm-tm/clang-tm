#include "expli_tm_api/tm_api.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

// ── Config ─────────────────────────────────────────────────────────────
struct Config {
    int threads = 4;
    int duration = 10000;
    int r1 = 10, w1 = 10, r2 = 10, w2 = 10;
    int a1 = 100, a2 = 10000, a3 = 10000;
    double contention = 0.5, locality = 0.5, density = 0.5;
    bool r2_enabled = true, r3_enabled = false;
    int mode = 0;
};

Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        auto nxt = [&](){ return i+1 < argc ? atoi(argv[++i]) : 0; };
        auto nxt_d = [&](){ return i+1 < argc ? atof(argv[++i]) : 0.0; };
        if      (!strcmp(argv[i], "-t"))  c.threads = nxt();
        else if (!strcmp(argv[i], "-d"))  c.duration = nxt();
        else if (!strcmp(argv[i], "--r1")) c.r1 = nxt();
        else if (!strcmp(argv[i], "--w1")) c.w1 = nxt();
        else if (!strcmp(argv[i], "--r2")) c.r2 = nxt();
        else if (!strcmp(argv[i], "--w2")) c.w2 = nxt();
        else if (!strcmp(argv[i], "--a1")) c.a1 = nxt();
        else if (!strcmp(argv[i], "--a2")) c.a2 = nxt();
        else if (!strcmp(argv[i], "--a3")) c.a3 = nxt();
        else if (!strcmp(argv[i], "--contention")) c.contention = nxt_d();
        else if (!strcmp(argv[i], "--locality")) c.locality = nxt_d();
        else if (!strcmp(argv[i], "--density")) c.density = nxt_d();
        else if (!strcmp(argv[i], "--mode")) c.mode = nxt();
        else if (!strcmp(argv[i], "--enable-r2")) c.r2_enabled = true;
        else if (!strcmp(argv[i], "--enable-r3")) c.r3_enabled = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: eigenbench [-t n] [-d ms] [--r1 n] [--w1 n] [--r2 n] [--w2 n]\n"
                   "  [--a1 n] [--a2 n] [--a3 n] [--contention f] [--locality f]\n"
                   "  [--density f] [--mode n] [--enable-r2] [--enable-r3]\n");
            exit(0);
        }
    }
    return c;
}

// ── Simple RNG ─────────────────────────────────────────────────────────
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 33;
    }
    uint64_t range(uint64_t lo, uint64_t hi) { return lo + next() % (hi - lo); }
};

// ── Shared arrays ──────────────────────────────────────────────────────
struct SharedArrays {
    std::vector<expli::TM<int64_t>> a1;
    std::vector<std::vector<expli::TM<int64_t>>> a2;
    std::vector<std::vector<expli::TM<int64_t>>> a3;
    Config cfg;

    SharedArrays(const Config &c) : cfg(c) {
        a1.resize(c.a1);
        a2.resize(c.threads);
        a3.resize(c.threads);
        for (int t = 0; t < c.threads; ++t) {
            a2[t].resize(c.a2);
            a3[t].resize(c.a3);
        }
    }
};

// ── Workers ────────────────────────────────────────────────────────────
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_total_ops{0};

void run_worker(int tid, SharedArrays &arrays, const Config &cfg) {
    expli::TM<int64_t>::thread_init();
    Rng rng(tid * 12345ULL + 42);

    std::vector<int> r1_idxs, w1_idxs;
    r1_idxs.reserve(cfg.r1);
    w1_idxs.reserve(cfg.w1);
    for (int i = 0; i < cfg.r1; ++i) {
        r1_idxs.push_back(cfg.contention > 0.5 ? tid % cfg.a1
                         : ((tid * 137 + tid * tid) % cfg.a1));
    }
    for (int i = 0; i < cfg.w1; ++i) {
        w1_idxs.push_back(cfg.contention > 0.5 ? tid % cfg.a1
                         : ((tid * 173 + tid * tid) % cfg.a1));
    }

    while (!g_stop.load()) {
        expli::TM<int64_t>::transaction([&]() {
            int64_t sum = 0;

            // R1: shared array reads
            for (int idx : r1_idxs)
                sum += arrays.a1[idx % cfg.a1].read();

            // W1: shared array writes
            for (int j = 0; j < (int)w1_idxs.size(); ++j)
                arrays.a1[w1_idxs[j] % cfg.a1].write(sum + j);

            // R2/W2: thread-local array
            if (cfg.r2_enabled) {
                int base = tid * cfg.a2;
                for (int j = 0; j < cfg.r2; ++j) {
                    int idx;
                    if (cfg.locality > 0.8)
                        idx = base + j % cfg.a2;
                    else if (cfg.locality > 0.5)
                        idx = base + r1_idxs[j % r1_idxs.size()] % cfg.a2;
                    else
                        idx = base + (j * 137) % cfg.a2;
                    sum += arrays.a2[tid][idx % cfg.a2].read();
                }
                for (int j = 0; j < cfg.w2; ++j) {
                    int idx;
                    if (cfg.density > 0.8)
                        idx = base + j;
                    else if (cfg.density > 0.5)
                        idx = base + w1_idxs[j % w1_idxs.size()] % cfg.a2;
                    else
                        idx = base + (j * 173) % cfg.a2;
                    arrays.a2[tid][idx % cfg.a2].write(sum);
                }
            }
        });
        g_total_ops.fetch_add(1);

        // R3: outer-loop non-TM ops
        if (cfg.r3_enabled && cfg.mode != 1) {
            int limit = cfg.a3 < 100 ? cfg.a3 : 100;
            for (int j = 0; j < limit; ++j) {
                int idx = cfg.locality > 0.8 ? j % cfg.a3 : (j * 137) % cfg.a3;
                int64_t v = arrays.a3[tid][idx].peek();
                arrays.a3[tid][idx].poke(v + 1);
        }
    }
    }
    expli::TM<int64_t>::thread_exit();
}

// ── Main ───────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    Config cfg = parse_args(argc, argv);

    printf("========= EigenBench =========\n");
    printf("===============================\n");
    printf("Threads: %d  Duration: %dms\n", cfg.threads, cfg.duration);
    printf("R1: %d  W1: %d  R2: %d  W2: %d\n", cfg.r1, cfg.w1, cfg.r2, cfg.w2);
    printf("A1: %d  A2: %d  A3: %d\n", cfg.a1, cfg.a2, cfg.a3);
    printf("Contention: %.2f  Locality: %.2f  Density: %.2f\n",
           cfg.contention, cfg.locality, cfg.density);
    printf("R2 enabled: %d  R3 enabled: %d  Mode: %d\n\n",
           cfg.r2_enabled, cfg.r3_enabled, cfg.mode);

    expli::TM<int64_t>::init();

    SharedArrays arrays(cfg);

    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; ++t)
        threads.emplace_back([t, &arrays, &cfg]() {
            run_worker(t, arrays, cfg);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.duration));
    g_stop.store(true);
    for (auto &th : threads) th.join();

    uint64_t ops = g_total_ops.load();
    double secs = cfg.duration / 1000.0;

    printf("\nResults\n");
    printf("=======\n");
    printf("Total TXs: %llu\n", (unsigned long long)ops);
    printf("Throughput: %.0f tx/s\n", ops / secs);

    expli::TM<int64_t>::exit();
    return 0;
}
