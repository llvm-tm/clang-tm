// STAMP/ssca2 benchmark — explicit TM API port
// Matches the plugin ssca2_bench.hpp algorithm.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <set>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <set>
#include <random>

static long g_scale = 13;
static long g_iterations = 10;
static double g_prob_unidirectional = 0.5;
static long g_max_paral_edges = 3;
static long g_num_threads = 4;

// ── TM abstraction (expli only) ────────────────────────────────────
extern "C" {
    void     tm_begin();
    void     tm_end();
    long     tm_read_i8(const long*);
    void     tm_write_i8(long*, long);
    void     tm_init();
    void     tm_exit();
    void     tm_init_thread();
    void     tm_exit_thread();
    void*    tm_calloc(size_t, size_t);
}
extern __thread int32_t tm_nested_call_counter;
extern __thread sigjmp_buf tm_jmpbuf;

#define TX_FUNC
#define TM_READ_I8(p)     tm_read_i8((const long*)(p))
#define TM_WRITE_I8(p, v) tm_write_i8((long*)(p), (long)(v))

template<typename F>
static void tx_run(F&& body) {
    volatile bool done = false;
    while (!done) {
        sigsetjmp(tm_jmpbuf, 0);
        tm_nested_call_counter = 1;
        tm_begin();
        body();
        tm_end();
        done = true;
    }
    tm_nested_call_counter = 0;
}

using PRNG = std::mt19937_64;

struct Edge {
    uint64_t src;
    uint64_t dst;
    int64_t weight;
};

struct SparseRow {
    std::vector<uint64_t> row_ptr;
    std::vector<uint64_t> col_idx;
    std::vector<int64_t> weights;
};

struct SSCA2Data {
    std::vector<Edge> edges;
    SparseRow graph;
    uint64_t num_vertices;
    uint64_t num_edges;
    int scale;
    int max_paral_edges;
    double perc_int_weights;
    double prob_unidirectional;
    double prob_intercl_edges;
    int subgr_edge_length;
};

static SSCA2Data g_data;
static std::atomic<uint64_t> g_total_ops{0};

// ── Edge queries (read-only, TM-wrapped for faithfulness) ─
static bool ssca2_has_edge(uint64_t src, uint64_t dst) {
    bool found = false;
    tx_retry([&]() {
        auto& row_ptr = g_data.graph.row_ptr;
        auto& col_idx = g_data.graph.col_idx;
        uint64_t start = row_ptr[src];
        uint64_t end = row_ptr[src + 1];
        found = false;
        for (uint64_t i = start; i < end; i++) {
            if (col_idx[i] == dst) { found = true; break; }
        }
    });
    return found;
}

// ── Build CSR from edge list ──────────────────────────────
static void build_csr() {
    auto& edges = g_data.edges;
    if (edges.empty()) return;

    std::sort(edges.begin(), edges.end(),
              [](const Edge& a, const Edge& b) {
                  if (a.src != b.src) return a.src < b.src;
                  return a.dst < b.dst;
              });

    auto last = std::unique(edges.begin(), edges.end(),
                            [](const Edge& a, const Edge& b) {
                                return a.src == b.src && a.dst == b.dst;
                            });
    edges.erase(last, edges.end());

    uint64_t max_v = 0;
    for (auto& e : edges)
        max_v = std::max(max_v, std::max(e.src, e.dst));
    g_data.num_vertices = max_v + 1;

    auto& row_ptr = g_data.graph.row_ptr;
    auto& col_idx = g_data.graph.col_idx;
    auto& weights = g_data.graph.weights;

    row_ptr.assign(g_data.num_vertices + 1, 0);
    for (auto& e : edges)
        row_ptr[e.src + 1]++;

    for (uint64_t i = 1; i <= g_data.num_vertices; i++)
        row_ptr[i] += row_ptr[i - 1];

    col_idx.resize(edges.size());
    weights.resize(edges.size());

    std::vector<uint64_t> temp_pos = row_ptr;
    for (auto& e : edges) {
        uint64_t pos = temp_pos[e.src]++;
        col_idx[pos] = e.dst;
        weights[pos] = e.weight;
    }
}

// ── Worker thread ─────────────────────────────────────────
static void worker(int thread_id, int num_threads) {
    expli::TM<int>::thread_init();

    auto& row_ptr = g_data.graph.row_ptr;
    auto& col_idx = g_data.graph.col_idx;

    uint64_t chunk = (g_data.num_vertices + num_threads - 1) / num_threads;
    uint64_t start_v = thread_id * chunk;
    uint64_t end_v = std::min(start_v + chunk, g_data.num_vertices);
    uint64_t local_ops = 0;

    for (int iter = 0; iter < 3; iter++) {
        for (uint64_t v = start_v; v < end_v; v++) {
            uint64_t start = row_ptr[v];
            uint64_t end = row_ptr[v + 1];

            for (uint64_t i = start; i < end; i++) {
                uint64_t neighbor = col_idx[i];
                uint64_t nstart = row_ptr[neighbor];
                uint64_t nend = row_ptr[neighbor + 1];

                for (uint64_t j = nstart; j < nend; j++) {
                    uint64_t n2 = col_idx[j];
                    bool triangle = ssca2_has_edge(n2, v);
                    if (triangle)
                        local_ops++;
                }
            }
        }
    }

    g_total_ops.fetch_add(local_ops, std::memory_order_relaxed);
    expli::TM<int>::thread_exit();
}

int main(int argc, char* argv[]) {
    int num_threads = 4;
    int scale = 14;
    double prob_unidirectional = 1.0;
    int max_paral_edges = 3;
    int subgr_edge_length = 3;
    int iterations = 3;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) prob_unidirectional = atof(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) subgr_edge_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) max_paral_edges = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) iterations = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -s <scale> -u <prob> -l <len> -m <par_edges> -i <iters>\n", argv[0]);
            return 0;
        }
    }

    rng_seed(42);

    g_data.scale = scale;
    g_data.max_paral_edges = max_paral_edges;
    g_data.perc_int_weights = 0.6;
    g_data.prob_unidirectional = prob_unidirectional;
    g_data.prob_intercl_edges = 0.5;
    g_data.subgr_edge_length = subgr_edge_length;

    uint64_t tot_vertices = (uint64_t)1 << scale;
    int max_clique_size = 1 << (scale / 3);

    PRNG rng(42);

    std::vector<int> perm(tot_vertices);
    for (uint64_t i = 0; i < tot_vertices; i++) perm[i] = (int)i;
    for (uint64_t i = tot_vertices - 1; i > 0; i--) {
        uint64_t j = rng() % (i + 1);
        std::swap(perm[i], perm[j]);
    }

    std::vector<int> clique_sizes;
    uint64_t assigned = 0;
    while (assigned < tot_vertices) {
        int sz = (int)(rng() % max_clique_size) + 1;
        if (assigned + (uint64_t)sz > tot_vertices)
            sz = (int)(tot_vertices - assigned);
        clique_sizes.push_back(sz);
        assigned += sz;
    }

    uint64_t start_v = 0;
    std::set<std::pair<uint64_t, uint64_t>> edge_set;
    std::vector<Edge> temp_edges;

    for (size_t c = 0; c < clique_sizes.size(); c++) {
        int csize = clique_sizes[c];
        for (int i = 0; i < csize; i++) {
            for (int j = 0; j < csize; j++) {
                if (i == j) continue;
                double u = (double)rng() / (double)rng.max();
                if (u >= prob_unidirectional) {
                    uint64_t si = (uint64_t)perm[start_v + i];
                    uint64_t sj = (uint64_t)perm[start_v + j];
                    auto p = std::make_pair(si, sj);
                    if (edge_set.insert(p).second) {
                        double wu = (double)rng() / (double)rng.max();
                        int64_t w = (wu < g_data.perc_int_weights)
                            ? (int64_t)(rng() % (1 << scale))
                            : -(int64_t)(rng() % scale);
                        temp_edges.push_back({si, sj, w});
                    }
                }
            }
        }
        start_v += csize;
    }

    start_v = 0;
    for (size_t c = 0; c < clique_sizes.size(); c++) {
        int csize = clique_sizes[c];
        for (int i = 0; i < csize; i++) {
            uint64_t v = (uint64_t)perm[start_v + i];
            for (int d = 1; d < (int)tot_vertices; d *= 2) {
                double u = (double)rng() / (double)rng.max();
                if (u < g_data.prob_intercl_edges / (std::log2((double)d + 1.0) + 1.0)) {
                    uint64_t neighbor = (v + (uint64_t)d) % tot_vertices;
                    for (int p = 0; p < max_paral_edges; p++) {
                        double u2 = (double)rng() / (double)rng.max();
                        if (u2 < 0.5) {
                            auto pair = std::make_pair(v, neighbor);
                            if (edge_set.insert(pair).second) {
                                double wu = (double)rng() / (double)rng.max();
                                int64_t w = (wu < g_data.perc_int_weights)
                                    ? (int64_t)(rng() % (1 << scale))
                                    : -(int64_t)(rng() % scale);
                                temp_edges.push_back({v, neighbor, w});
                            }
                        }
                    }
                }
            }
        }
        start_v += csize;
    }

    for (auto& e : temp_edges) {
        e.src = (uint64_t)perm[e.src % tot_vertices];
        e.dst = (uint64_t)perm[e.dst % tot_vertices];
        g_data.edges.push_back(e);
    }

    build_csr();
    g_data.num_edges = g_data.edges.size();

    printf("Number of processors:       %i\n", num_threads);
    printf("Problem Scale:              %i\n", scale);
    printf("Max parallel edges:         %i\n", max_paral_edges);
    printf("Percent int weights:        %f\n", g_data.perc_int_weights);
    printf("Probability unidirectional: %f\n", prob_unidirectional);
    printf("Vertices: %lu  Edges: %lu\n",
           (unsigned long)g_data.num_vertices, (unsigned long)g_data.num_edges);
    fflush(stdout);

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++)
        threads.emplace_back(worker, i, num_threads);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    printf("    Time = %lld ms\n", (long long)elapsed);
    printf("    Triangles = %llu\n", (unsigned long long)ops);

    expli::TM<int>::exit();
    return 0;
}
