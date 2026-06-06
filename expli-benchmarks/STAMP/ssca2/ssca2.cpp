// STAMP/ssca2 benchmark — explicit TM API port
// Matches the plugin ssca2_bench.hpp algorithm.

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "../../tests/benchmark_test.hpp"

static long g_scale = 13;
static long g_iterations = 10;
static double g_prob_unidirectional = 0.5;
static long g_max_paral_edges = 3;
static long g_num_threads = 4;
static int g_subgr_edge_length = 3;

static void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) g_num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) g_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) g_prob_unidirectional = atof(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) g_subgr_edge_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) g_max_paral_edges = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) g_iterations = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -s <scale> -u <prob> -l <len> -m <par_edges> -i <iters>\n", argv[0]);
            exit(0);
        }
    }
}

template<typename F>
static void tx_retry(F&& body) {
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

    for (int iter = 0; iter < g_iterations; iter++) {
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

static void test_cli_flags() {
    printf("  Testing CLI flags...\n");
    long save_scale = g_scale, save_iters = g_iterations;
    double save_prob = g_prob_unidirectional;
    long save_par = g_max_paral_edges, save_sub = g_subgr_edge_length, save_t = g_num_threads;
    TEST_EQ(g_scale, 13L, "default scale");
    TEST_EQ(g_iterations, 10L, "default iterations");
    TEST_NEAR(g_prob_unidirectional, 0.5, 1e-9, "default prob");
    TEST_EQ(g_max_paral_edges, 3L, "default max paral edges");
    TEST_EQ(g_num_threads, 4L, "default threads");
    const char* test_args[] = {"prog", "-p", "2", "-s", "8", "-u", "0.3", "-l", "5", "-m", "1", "-i", "3"};
    parse_args(13, (char**)test_args);
    TEST_EQ(g_num_threads, 2L, "override threads");
    TEST_EQ(g_scale, 8L, "override scale");
    TEST_NEAR(g_prob_unidirectional, 0.3, 1e-9, "override prob");
    TEST_EQ(g_subgr_edge_length, 5, "override subgr edge len");
    TEST_EQ(g_max_paral_edges, 1L, "override max paral edges");
    TEST_EQ(g_iterations, 3L, "override iterations");
    g_scale = save_scale; g_iterations = save_iters;
    g_prob_unidirectional = save_prob; g_max_paral_edges = save_par;
    g_subgr_edge_length = save_sub; g_num_threads = save_t;
    if (test_result() != 0) exit(1);
}

static void test_rng() {
    printf("  Testing RNG determinism...\n");
    test_rng_determinism<PRNG>();
    if (test_result() != 0) exit(1);
}

static void test_logic() {
    printf("  Testing SSCA2 logic...\n");
    // Build a simple graph and verify CSR construction (no TM needed)
    // Create 4 vertices: 0-1, 1-2, 2-0 = triangle (0,1,2)
    g_data.edges.clear();
    g_data.edges.push_back({0, 1, 1});
    g_data.edges.push_back({1, 2, 1});
    g_data.edges.push_back({2, 0, 1});
    g_data.edges.push_back({1, 3, 1});
    build_csr();

    TEST_EQ((int)g_data.num_vertices, 4, "4 vertices");
    TEST_EQ((int)g_data.edges.size(), 4, "4 unique edges");

    // Check CSR structure directly (no TM calls)
    auto& row = g_data.graph.row_ptr;
    auto& col = g_data.graph.col_idx;
    TEST_EQ((int)row.size(), 5, "row_ptr has 5 entries");
    TEST_EQ((int)col.size(), 4, "col_idx has 4 entries");

    // Helper to check adjacency
    auto has_edge_csr = [&](uint64_t src, uint64_t dst) {
        uint64_t s = row[src], e = row[src + 1];
        for (uint64_t i = s; i < e; i++)
            if (col[i] == dst) return true;
        return false;
    };
    TEST_ASSERT(has_edge_csr(0, 1), "CSR edge 0->1");
    TEST_ASSERT(has_edge_csr(1, 2), "CSR edge 1->2");
    TEST_ASSERT(has_edge_csr(2, 0), "CSR edge 2->0");
    TEST_ASSERT(has_edge_csr(1, 3), "CSR edge 1->3");
    TEST_ASSERT(!has_edge_csr(0, 2), "CSR no edge 0->2");

    // Manual triangle detection without TM
    bool found_triangle = false;
    for (uint64_t a = 0; a < g_data.num_vertices && !found_triangle; a++) {
        for (uint64_t i = row[a]; i < row[a + 1] && !found_triangle; i++) {
            uint64_t b = col[i];
            for (uint64_t j = row[b]; j < row[b + 1] && !found_triangle; j++) {
                uint64_t c = col[j];
                if (has_edge_csr(c, a)) found_triangle = true;
            }
        }
    }
    TEST_ASSERT(found_triangle, "triangle (0,1,2) found via CSR");

    g_data.edges.clear();
    g_data.graph.row_ptr.clear();
    g_data.graph.col_idx.clear();
    g_data.graph.weights.clear();
    if (test_result() != 0) exit(1);
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("Running self-tests for ssca2...\n");
        test_cli_flags();
        test_rng();
        test_logic();
        printf("All tests passed.\n");
        return 0;
    }
    parse_args(argc, argv);

    int num_threads = (int)g_num_threads;
    int scale = (int)g_scale;
    double prob_unidirectional = g_prob_unidirectional;
    int max_paral_edges = (int)g_max_paral_edges;
    int subgr_edge_length = g_subgr_edge_length;

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

    expli::TM<int>::init();

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
    printf("Time taken for all is %f sec.\n", elapsed / 1000.0);

    expli::TM<int>::exit();
    return 0;
}
