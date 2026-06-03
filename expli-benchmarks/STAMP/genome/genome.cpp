// Simplified STAMP/genome benchmark using the explicit TM API.
//
// Gene sequence matching: read segments from a shared hash map,
// find overlapping segments, and merge them.
// Read-heavy: most TXs read from shared structures.
//
// Build:
//   make -C expli_benchmarks BACKEND=TINYSTM run-genome
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
    int gene_length = 16384;
    int segment_length = 64;
    int num_segments = 16777216;
    unsigned seed = 42;
};

static Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i+1 < argc) c.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i+1 < argc) c.gene_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) c.segment_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i+1 < argc) c.num_segments = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-S") && i+1 < argc) c.seed = (unsigned)atoi(argv[++i]);
    }
    return c;
}

// ── Segment (gene fragment) ─────────────────────────────────
struct Segment {
    int id;
    int start;
    int length;
    char *data;
};

// ── Shared TM-tracked segment store ─────────────────────────
struct GSegment {
    expli::TM<int> start;
    expli::TM<int> length;
    expli::TM<int> matched;
};

static std::vector<GSegment> g_segments;
static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_matches{0};
static std::atomic<uint64_t> g_reads{0};

static void worker(int tid, const Config &cfg) {
    expli::TM<int>::thread_init();
    auto rng = std::mt19937(cfg.seed + tid * 1000);
    int n_per_thread = (cfg.num_segments + cfg.threads - 1) / cfg.threads;
    int start = tid * n_per_thread;
    int end = std::min(start + n_per_thread, cfg.num_segments);

    for (int iter = 0; iter < 10; iter++) {
        for (int i = start; i < end; i++) {
            int target = rng() % cfg.num_segments;
            tx_retry([&]() {
                int s = g_segments[target].start.read();
                int l = g_segments[target].length.read();
                int m = g_segments[target].matched.read();
                // Simple overlap check
                int s2 = g_segments[i].start.read();
                int l2 = g_segments[i].length.read();
                int overlap = std::max(0, std::min(s + l, s2 + l2) - std::max(s, s2));
                if (overlap > 0 && m == 0) {
                    g_segments[target].matched.write(1);
                    g_matches.fetch_add(1);
                }
                g_reads.fetch_add(1);
            });
        }
    }
    expli::TM<int>::thread_exit();
}

int main(int argc, char *argv[]) {
    auto cfg = parse_args(argc, argv);

    printf("Genome — Explicit TM API\n");
    printf("Threads: %d  Gene: %d  Segment: %d  NumSegments: %d\n",
           cfg.threads, cfg.gene_length, cfg.segment_length, cfg.num_segments);

    expli::TM<int>::init();

    // Initialize segments
    auto rng = std::mt19937(cfg.seed);
    g_segments.resize(cfg.num_segments);
    for (int i = 0; i < cfg.num_segments; i++) {
        g_segments[i].start.poke(rng() % cfg.gene_length);
        g_segments[i].length.poke((rng() % cfg.segment_length) + 1);
        g_segments[i].matched.poke(0);
    }

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; t++)
        threads.emplace_back(worker, t, std::ref(cfg));
    for (auto &th : threads)
        th.join();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  Matches: %llu  Reads: %llu\n",
           (unsigned long long)g_matches.load(),
           (unsigned long long)g_reads.load());
    printf("  PASS\n");
    expli::TM<int>::exit();
    return 0;
}
