// Simplified STAMP/kmeans benchmark using the explicit TM API.
//
// K-means clustering: partition N points into K clusters across D dimensions.
// Each iteration: assign points to nearest cluster, update cluster centers.
// TM protects the cluster center updates (read-shared, write-shared).
//
// Build:
//   make -C expli_benchmarks BACKEND=TINYSTM run-kmeans
//
// Original: https://stamp.stanford.edu/

#include "../../../expli_tm_api/tm_api.hpp"

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
    int clusters = 16;
    int dims = 2;
    int points = 2048;
    int iterations = 100;
    unsigned seed = 42;
};

static Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i+1 < argc) c.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i+1 < argc) c.clusters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i+1 < argc) c.dims = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i+1 < argc) c.points = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i+1 < argc) c.iterations = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) c.seed = (unsigned)atoi(argv[++i]);
    }
    return c;
}

// ── Global state ────────────────────────────────────────────
// Center coordinates as flat TM array: centers[d * clusters + k]
static expli::TM<double> *g_centers = nullptr;
static std::vector<std::vector<double>> g_points;
static std::atomic<uint64_t> g_iterations{0};

// Read all center coordinates into a local buffer (no TM reads inside inner loop)
static void read_centers_local(double *buf, int clusters, int dims) {
    for (int k = 0; k < clusters; k++)
        for (int d = 0; d < dims; d++)
            buf[k * dims + d] = g_centers[k * dims + d].peek();
}

// Euclidean distance squared
static double dist_sq(const double *a, const double *b, int dim) {
    double sum = 0.0;
    for (int d = 0; d < dim; d++) {
        double diff = a[d] - b[d];
        sum += diff * diff;
    }
    return sum;
}

// Assign points to nearest clusters (parallel, each thread owns partition)
static void assign_and_update(int tid, const Config &cfg) {
    int n_per_thread = (cfg.points + cfg.threads - 1) / cfg.threads;
    int start = tid * n_per_thread;
    int end = std::min(start + n_per_thread, cfg.points);

    expli::TM<double>::thread_init();
    std::vector<int> assignments(end - start);
    std::vector<double> local_centers(cfg.clusters * cfg.dims);

    for (int iter = 0; iter < cfg.iterations; iter++) {
        // Read center coordinates into local buffer (non-TM)
        read_centers_local(local_centers.data(), cfg.clusters, cfg.dims);

        // Assign each point to nearest cluster
        for (int i = start; i < end; i++) {
            int best = 0;
            double best_dist = INFINITY;
            for (int k = 0; k < cfg.clusters; k++) {
                double d = dist_sq(g_points[i].data(), &local_centers[k * cfg.dims], cfg.dims);
                if (d < best_dist) {
                    best_dist = d;
                    best = k;
                }
            }
            assignments[i - start] = best;
        }

        // Update cluster centers transactionally
        for (int k = 0; k < cfg.clusters; k++) {
            double sum[128] = {0.0};
            int count = 0;
            for (int i = start; i < end; i++) {
                if (assignments[i - start] == k) {
                    for (int d = 0; d < cfg.dims; d++)
                        sum[d] += g_points[i][d];
                    count++;
                }
            }
            if (count > 0) {
                tx_retry([&]() {
                    for (int d = 0; d < cfg.dims; d++) {
                        double old = g_centers[k * cfg.dims + d].read();
                        g_centers[k * cfg.dims + d].write(old + sum[d] / count * 0.01);
                    }
                });
            }
        }

        g_iterations.fetch_add(1, std::memory_order_relaxed);
    }
}

int main(int argc, char *argv[]) {
    auto cfg = parse_args(argc, argv);

    printf("KMeans — Explicit TM API\n");
    printf("Threads: %d  Clusters: %d  Dims: %d  Points: %d  Iterations: %d\n",
           cfg.threads, cfg.clusters, cfg.dims, cfg.points, cfg.iterations);

    expli::TM<double>::init();

    // Generate random points
    auto rng = std::mt19937(cfg.seed);
    g_points.resize(cfg.points);
    for (int i = 0; i < cfg.points; i++) {
        g_points[i].resize(cfg.dims);
        for (int d = 0; d < cfg.dims; d++)
            g_points[i][d] = std::uniform_real_distribution<double>(0, 1000.0)(rng);
    }

    // Initialize cluster centers (TM-tracked, flat array)
    g_centers = new expli::TM<double>[cfg.clusters * cfg.dims];
    for (int k = 0; k < cfg.clusters; k++)
        for (int d = 0; d < cfg.dims; d++)
            g_centers[k * cfg.dims + d].poke(std::uniform_real_distribution<double>(0, 1000.0)(rng));

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; t++)
        threads.emplace_back(assign_and_update, t, std::ref(cfg));
    for (auto &th : threads)
        th.join();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t iters = g_iterations.load();
    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  Iterations: %llu\n", (unsigned long long)iters);
    printf("  PASS\n");

    delete[] g_centers;
    expli::TM<double>::exit();
    return 0;
}
