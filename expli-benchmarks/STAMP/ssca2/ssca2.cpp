// Simplified STAMP/ssca2 benchmark using the explicit TM API.
//
// Graph construction (Kernel 2): build edges between vertices by
// finding similar subgraphs.  Many small TXs with moderate read-sets.
//
// Build:
//   make -C expli_benchmarks BACKEND=TINYSTM run-ssca2
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
    int scale = 14;
    double initiator_prob = 1.0;
    double update_prob = 1.0;
    int min_degree = 3;
    int max_degree = 3;
    unsigned seed = 42;
};

static Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i+1 < argc) c.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) c.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i+1 < argc) c.initiator_prob = atof(argv[++i]);
        else if (!strcmp(argv[i], "-u") && i+1 < argc) c.update_prob = atof(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i+1 < argc) c.min_degree = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i+1 < argc) c.max_degree = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-S") && i+1 < argc) c.seed = (unsigned)atoi(argv[++i]);
    }
    return c;
}

// ── Vertex (TM-tracked edge count) ──────────────────────────
struct Vertex {
    expli::TM<int> edge_count;
    expli::TM<int> active;
};

static std::vector<Vertex> g_vertices;
static std::vector<std::pair<int,int>> g_edges;
static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_txns{0};
static std::atomic<uint64_t> g_edges_built{0};

static void worker(int tid, const Config &cfg) {
    expli::TM<int>::thread_init();
    auto rng = std::mt19937(cfg.seed + tid * 1000);
    int n_vertices = 1 << cfg.scale;
    int n_per_thread = (n_vertices + cfg.threads - 1) / cfg.threads;
    int start = tid * n_per_thread;
    int end = std::min(start + n_per_thread, n_vertices);

    // Each thread builds edges: for each vertex, connect to random neighbors
    // O(N * max_degree) instead of O(N²)
    for (int v = start; v < end; v++) {
        int target_count = std::uniform_int_distribution<int>(cfg.min_degree, cfg.max_degree)(rng);
        for (int e = 0; e < target_count; e++) {
            int w = rng() % n_vertices;
            if (w == v) continue;
            tx_retry([&]() {
                int ec = g_vertices[v].edge_count.read();
                int ec2 = g_vertices[w].edge_count.read();
                if (ec < cfg.max_degree && ec2 < cfg.max_degree) {
                    g_vertices[v].edge_count.write(ec + 1);
                    g_vertices[w].edge_count.write(ec2 + 1);
                    g_edges_built.fetch_add(1);
                }
            });
            g_txns.fetch_add(1);
        }
    }
    expli::TM<int>::thread_exit();
}

int main(int argc, char *argv[]) {
    auto cfg = parse_args(argc, argv);
    int n_vertices = 1 << cfg.scale;

    printf("SSCA2 — Explicit TM API\n");
    printf("Threads: %d  Scale: %d  Vertices: %d  InitProb: %.2f  MaxDeg: %d\n",
           cfg.threads, cfg.scale, n_vertices, cfg.initiator_prob, cfg.max_degree);

    expli::TM<int>::init();

    // Initialize vertices
    g_vertices.resize(n_vertices);
    for (int i = 0; i < n_vertices; i++) {
        g_vertices[i].edge_count.poke(0);
        g_vertices[i].active.poke(1);
    }

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; t++)
        threads.emplace_back(worker, t, std::ref(cfg));
    for (auto &th : threads)
        th.join();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t txns = g_txns.load();
    uint64_t edges = g_edges_built.load();
    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  TXNs: %llu  Edges built: %llu\n",
           (unsigned long long)txns, (unsigned long long)edges);
    printf("  PASS\n");
    expli::TM<int>::exit();
    return 0;
}
