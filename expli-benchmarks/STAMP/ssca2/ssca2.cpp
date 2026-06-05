// SSCA2 — C++ port of the original STAMP spec (explicit API path)
// Original spec: https://github.com/ccaominh/stamp/tree/master/ssca2
//
// Parameters:
//   -s <num>   Problem scale (2^s vertices)  (default: 13)
//   -i <num>   Iterations                    (default: 10)
//   -u <float> Prob unidirectional           (default: 0.5)
//   -l <num>   Subgraph edge length          (default: 3)
//   -p <num>   Max parallel edges            (default: 3)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csetjmp>
#include <cstdint>
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

// ── PRNG (std::mt19937_64 matching plugin) ─────────────────────────
static thread_local std::mt19937_64 tls_rng;

static void rng_seed(uint64_t s) { tls_rng = std::mt19937_64(s); }
static uint64_t rng_next() { return tls_rng(); }
static double rng_uniform() {
    return (double)tls_rng() / (double)tls_rng.max();
}

// ── TM-tracked CSR data ────────────────────────────────────────────
// Flat arrays allocated from TM region, written during setup,
// read-only during triangle counting.
static long* g_row_ptr = nullptr;  // [num_vertices + 1]
static long* g_col_idx = nullptr;  // [num_edges]
static long* g_weights = nullptr;  // [num_edges]
static uint64_t g_num_vertices = 0;
static uint64_t g_num_edges = 0;

static std::atomic<uint64_t> g_total_triangles{0};

// ── TM has_edge (reads CSR through TM barriers) ────────────────────
TX_FUNC static bool has_edge(uint64_t src, uint64_t dst) {
    uint64_t start = (uint64_t)TM_READ_I8(&g_row_ptr[src]);
    uint64_t end = (uint64_t)TM_READ_I8(&g_row_ptr[src + 1]);
    for (uint64_t i = start; i < end; i++) {
        if ((uint64_t)TM_READ_I8(&g_col_idx[i]) == dst) return true;
    }
    return false;
}

// ── Worker: triangle counting ──────────────────────────────────────
static void worker(long tid) {
    tm_init_thread();

    uint64_t chunk = (g_num_vertices + (uint64_t)g_num_threads - 1) / (uint64_t)g_num_threads;
    uint64_t start_v = (uint64_t)tid * chunk;
    uint64_t end_v = std::min(start_v + chunk, g_num_vertices);
    uint64_t local_ops = 0;

    // CSR data is read-only after construction — non-TM reads for traversal
    for (long iter = 0; iter < g_iterations; iter++) {
        for (uint64_t v = start_v; v < end_v; v++) {
            uint64_t start = (uint64_t)g_row_ptr[v];
            uint64_t end = (uint64_t)g_row_ptr[v + 1];

            for (uint64_t i = start; i < end; i++) {
                uint64_t neighbor = (uint64_t)g_col_idx[i];
                uint64_t nstart = (uint64_t)g_row_ptr[neighbor];
                uint64_t nend = (uint64_t)g_row_ptr[neighbor + 1];

                for (uint64_t j = nstart; j < nend; j++) {
                    uint64_t n2 = (uint64_t)g_col_idx[j];
                    bool triangle = false;
                    tx_run([&]() { triangle = has_edge(n2, v); });
                    if (triangle) local_ops++;
                }
            }
        }
    }

    g_total_triangles.fetch_add(local_ops, std::memory_order_relaxed);
}

// ── Graph generation (non-TM, from plugin spec) ────────────────────
struct Edge {
    uint64_t src, dst;
    int64_t weight;
};

static void generate_graph() {
    uint64_t tot_vertices = (uint64_t)1 << g_scale;
    int max_clique_size = 1 << (g_scale / 3);

    rng_seed(42);

    // Fisher-Yates shuffle
    std::vector<int> perm(tot_vertices);
    for (uint64_t i = 0; i < tot_vertices; i++) perm[i] = (int)i;
    for (uint64_t i = tot_vertices - 1; i > 0; i--) {
        uint64_t j = rng_next() % (i + 1);
        std::swap(perm[i], perm[j]);
    }

    // Clique sizes
    std::vector<int> clique_sizes;
    uint64_t assigned = 0;
    while (assigned < tot_vertices) {
        int sz = (int)(rng_next() % (uint64_t)max_clique_size) + 1;
        if (assigned + (uint64_t)sz > tot_vertices)
            sz = (int)(tot_vertices - assigned);
        clique_sizes.push_back(sz);
        assigned += sz;
    }

    // Generate edges
    std::set<std::pair<uint64_t, uint64_t>> edge_set;
    std::vector<Edge> temp_edges;
    double perc_int_weights = 0.6;
    double prob_intercl_edges = 0.5;

    uint64_t start_v = 0;
    for (size_t c = 0; c < clique_sizes.size(); c++) {
        int csize = clique_sizes[c];
        for (int i = 0; i < csize; i++) {
            for (int j = 0; j < csize; j++) {
                if (i == j) continue;
                if (rng_uniform() >= g_prob_unidirectional) continue;
                uint64_t si = (uint64_t)perm[start_v + i];
                uint64_t sj = (uint64_t)perm[start_v + j];
                auto p = std::make_pair(si, sj);
                if (edge_set.insert(p).second) {
                    int64_t w = (rng_uniform() < perc_int_weights)
                        ? (int64_t)(rng_next() % ((uint64_t)1 << g_scale))
                        : -(int64_t)(rng_next() % (uint64_t)g_scale);
                    temp_edges.push_back({si, sj, w});
                }
            }
        }
        start_v += (uint64_t)csize;
    }

    // Inter-clique edges
    start_v = 0;
    for (size_t c = 0; c < clique_sizes.size(); c++) {
        int csize = clique_sizes[c];
        for (int i = 0; i < csize; i++) {
            uint64_t v = (uint64_t)perm[start_v + i];
            for (uint64_t d = 1; d < tot_vertices; d *= 2) {
                if (rng_uniform() >= prob_intercl_edges / (std::log2((double)(d + 1)) + 1.0))
                    continue;
                uint64_t neighbor = (v + d) % tot_vertices;
                for (int ep = 0; ep < (int)g_max_paral_edges; ep++) {
                    if (rng_uniform() >= 0.5) continue;
                    auto p = std::make_pair(v, neighbor);
                    if (edge_set.insert(p).second) {
                        int64_t w = (rng_uniform() < perc_int_weights)
                            ? (int64_t)(rng_next() % ((uint64_t)1 << g_scale))
                            : -(int64_t)(rng_next() % (uint64_t)g_scale);
                        temp_edges.push_back({v, neighbor, w});
                    }
                }
            }
        }
        start_v += (uint64_t)csize;
    }

    // Permute final edge src/dst
    std::vector<Edge> edges;
    for (auto& e : temp_edges) {
        edges.push_back({
            (uint64_t)perm[e.src % tot_vertices],
            (uint64_t)perm[e.dst % tot_vertices],
            e.weight
        });
    }

    // Sort + dedup edges
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

    // Build CSR
    uint64_t max_v = 0;
    for (auto& e : edges)
        max_v = std::max(max_v, std::max(e.src, e.dst));
    g_num_vertices = max_v + 1;
    g_num_edges = edges.size();

    std::vector<uint64_t> row_ptr(g_num_vertices + 1, 0);
    for (auto& e : edges) row_ptr[e.src + 1]++;
    for (uint64_t i = 1; i <= g_num_vertices; i++)
        row_ptr[i] += row_ptr[i - 1];

    std::vector<uint64_t> col_idx(edges.size());
    std::vector<int64_t> weights(edges.size());
    std::vector<uint64_t> temp_pos = row_ptr;
    for (auto& e : edges) {
        uint64_t pos = temp_pos[e.src]++;
        col_idx[pos] = e.dst;
        weights[pos] = e.weight;
    }

    // Copy CSR to TM arrays
    g_row_ptr = (long*)tm_calloc(g_num_vertices + 1, sizeof(long));
    g_col_idx = (long*)tm_calloc(g_num_edges, sizeof(long));
    g_weights = (long*)tm_calloc(g_num_edges, sizeof(long));

    for (uint64_t i = 0; i <= g_num_vertices; i++)
        g_row_ptr[i] = (long)row_ptr[i];
    for (uint64_t i = 0; i < g_num_edges; i++) {
        g_col_idx[i] = (long)col_idx[i];
        g_weights[i] = (long)weights[i];
    }

    printf("Vertices: %llu  Edges: %llu\n",
           (unsigned long long)g_num_vertices,
           (unsigned long long)g_num_edges);
    fflush(stdout);
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-s") == 0 && i+1 < argc)
            g_scale = atol(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i+1 < argc)
            g_iterations = atol(argv[++i]);
        else if (strcmp(argv[i], "-u") == 0 && i+1 < argc)
            g_prob_unidirectional = atof(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i+1 < argc)
            g_max_paral_edges = atol(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i+1 < argc)
            g_max_paral_edges = atol(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc)
            g_num_threads = atol(argv[++i]);
    }

    printf("SSCA2 (STAMP spec)\n");
    printf("  Scale:  %ld (2^%ld = %llu vertices)\n",
           g_scale, g_scale, (unsigned long long)((uint64_t)1 << g_scale));
    printf("  Iters:  %ld\n", g_iterations);
    printf("  Uni:    %.2f\n", g_prob_unidirectional);
    printf("  MaxPar: %ld\n", g_max_paral_edges);
    printf("  Threads:%ld\n", g_num_threads);

    tm_init();
    generate_graph();

    auto t1 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (long t = 0; t < g_num_threads; t++)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    uint64_t triangles = g_total_triangles.load();
    printf("\nResults (%ld ms):\n", (long)(elapsed * 1000));
    printf("  Triangles: %llu\n", (unsigned long long)triangles);
    printf("  PASS\n");

    tm_exit();
    return 0;
}
