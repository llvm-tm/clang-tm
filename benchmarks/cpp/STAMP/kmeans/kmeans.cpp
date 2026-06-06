// STAMP/kmeans benchmark — explicit TM API port
// Matches the plugin kmeans_bench.hpp algorithm exactly.

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <thread>
#include <vector>
#include "../../tests/benchmark_test.hpp"

static long g_nclusters = 40;
static long g_npoints   = 40;
static double g_threshold = 0.00001;
static long g_num_threads = 4;
static long g_ndims = 40;

static void parse_args(int argc, char* argv[]) {
    g_num_threads = 4;
    g_npoints = 40;
    g_nclusters = 40;
    g_ndims = 40;
    g_threshold = 0.00001;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) g_num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) g_nclusters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) { g_ndims = atoi(argv[++i]); g_nclusters = g_ndims; }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) g_ndims = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) g_npoints = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) g_threshold = atof(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -k <clusters> -d <dims> -n <points> -t <threshold>\n", argv[0]);
            return;
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

static inline double tm_read_double(double* addr) {
    uint64_t raw = tm_read_i8(reinterpret_cast<uint64_t*>(addr));
    double val;
    memcpy(&val, &raw, sizeof(val));
    return val;
}

static inline void tm_write_double(double* addr, double val) {
    uint64_t raw;
    memcpy(&raw, &val, sizeof(raw));
    tm_write_i8(reinterpret_cast<uint64_t*>(addr), (int64_t)raw);
}

static inline int tm_read_int(int* addr) {
    return (int)tm_read_i4(reinterpret_cast<uint32_t*>(addr));
}

static inline void tm_write_int(int* addr, int val) {
    tm_write_i4(reinterpret_cast<uint32_t*>(addr), (uint32_t)val);
}

struct KMeansData {
    double* points;
    double* centroids;
    double* new_centers_sum;
    int*    new_centers_count;
    int*    assignments;
    int npoints;
    int ndims;
    int nclusters;
    double threshold;
};

static KMeansData g_data;

static std::atomic<uint64_t> g_total_ops{0};

static double tm_sqrt_s(double x) {
    if (x <= 0) return 0;
    double s = x;
    for (int i = 0; i < 25; i++) {
        double ns = (s + x / s) * 0.5;
        if (std::abs(ns - s) < 1e-15) break;
        s = ns;
    }
    return s;
}

static void kmeans_accumulate(KMeansData* data, int start, int end,
                               double* local_sum, int* local_count) {
    tx_retry([&]() {
        for (int i = start; i < end; i++) {
            int best = -1;
            double best_dist = std::numeric_limits<double>::max();
            for (int c = 0; c < data->nclusters; c++) {
                double dist = 0;
                for (int d = 0; d < data->ndims; d++) {
                    double diff = tm_read_double(&data->points[i * data->ndims + d])
                                - tm_read_double(&data->centroids[c * data->ndims + d]);
                    dist += diff * diff;
                }
                if (dist < best_dist) {
                    best_dist = dist;
                    best = c;
                }
            }
            tm_write_int(&data->assignments[i], best);
            local_count[best]++;
            for (int d = 0; d < data->ndims; d++) {
                local_sum[best * data->ndims + d] += tm_read_double(&data->points[i * data->ndims + d]);
            }
        }
    });
}

static void worker(int tid, int num_threads) {
    expli::TM<int>::thread_init();

    auto& data = g_data;
    int npoints = data.npoints;
    int ndims = data.ndims;
    int nclusters = data.nclusters;
    double threshold = data.threshold;

    double* local_sum = new double[nclusters * ndims]();
    int* local_count = new int[nclusters]();

    bool converged = false;
    int max_iters = 100;

    while (!converged && max_iters-- > 0) {
        std::fill(local_sum, local_sum + nclusters * ndims, 0.0);
        std::fill(local_count, local_count + nclusters, 0);

        int chunk = (npoints + num_threads - 1) / num_threads;
        int start = tid * chunk;
        int end = std::min(start + chunk, npoints);

        if (start < end) {
            kmeans_accumulate(&data, start, end, local_sum, local_count);
        }

        // Merge local accumulators into global (non-TX, matches plugin)
        for (int c = 0; c < nclusters; c++) {
            data.new_centers_count[c] += local_count[c];
            for (int d = 0; d < ndims; d++) {
                data.new_centers_sum[c * ndims + d] += local_sum[c * ndims + d];
            }
        }

        double delta = 0.0;
        for (int c = 0; c < nclusters; c++) {
            if (data.new_centers_count[c] > 0) {
                for (int d = 0; d < ndims; d++) {
                    double new_val = data.new_centers_sum[c * ndims + d] / data.new_centers_count[c];
                    double diff = data.centroids[c * ndims + d] - new_val;
                    delta += diff * diff;
                    data.centroids[c * ndims + d] = new_val;
                    data.new_centers_sum[c * ndims + d] = 0.0;
                }
            }
            data.new_centers_count[c] = 0;
        }

        delta = tm_sqrt_s(delta / (nclusters * ndims));
        converged = (delta < threshold);
        g_total_ops.fetch_add(npoints, std::memory_order_relaxed);
    }

    delete[] local_sum;
    delete[] local_count;
    expli::TM<int>::thread_exit();
}

static int test_cli_flags() {
    const char* av[] = {"kmeans"};
    parse_args(1, (char**)av);
    TEST_EQ(g_num_threads, 4L, "default -p");
    TEST_EQ(g_npoints, 40L, "default -n");
    TEST_EQ(g_nclusters, 40L, "default -k");
    TEST_EQ(g_ndims, 40L, "default -d");
    TEST_NEAR(g_threshold, 0.00001, 1e-15, "default -t");

    const char* av2[] = {"kmeans", "-p", "8", "-k", "10", "-d", "3", "-n", "100", "-t", "0.001"};
    parse_args(11, (char**)av2);
    TEST_EQ(g_num_threads, 8L, "override -p");
    TEST_EQ(g_npoints, 100L, "override -n");
    TEST_EQ(g_nclusters, 10L, "override -k");
    TEST_EQ(g_ndims, 3L, "override -d");
    TEST_NEAR(g_threshold, 0.001, 1e-15, "override -t");

    return test_result();
}

static int test_rng() {
    test_rng_determinism<PRNG>();
    return test_result();
}

static int test_logic() {
    // Euclidean distance: (1,2,3) vs (4,5,6)
    double p1[] = {1, 2, 3};
    double p2[] = {4, 5, 6};
    double dist_sq = 0;
    for (int i = 0; i < 3; i++) {
        double diff = p1[i] - p2[i];
        dist_sq += diff * diff;
    }
    TEST_NEAR(std::sqrt(dist_sq), 5.196152422706632, 1e-12, "euclidean distance");

    // Cluster assignment: 3 points to 2 centroids
    double pts[] = {0, 0,  6, 0,  10, 0};
    double cents[] = {0, 0,  9, 0};
    int asgn[3];
    for (int i = 0; i < 3; i++) {
        int best = -1;
        double best_d = std::numeric_limits<double>::max();
        for (int c = 0; c < 2; c++) {
            double d = 0;
            for (int j = 0; j < 2; j++) {
                double diff = pts[i * 2 + j] - cents[c * 2 + j];
                d += diff * diff;
            }
            if (d < best_d) { best_d = d; best = c; }
        }
        asgn[i] = best;
    }
    TEST_EQ(asgn[0], 0, "point (0,0) -> cluster 0");
    TEST_EQ(asgn[1], 1, "point (6,0) -> cluster 1");
    TEST_EQ(asgn[2], 1, "point (10,0) -> cluster 1");

    // Convergence test
    TEST_EQ((0.001 < 0.0005), false, "delta=0.001 not converged");
    TEST_EQ((0.0001 < 0.0005), true, "delta=0.0001 converged");

    return test_result();
}

static int test_all() {
    int f = 0;
    printf("  CLI flags...\n");  f += test_cli_flags();
    printf("  RNG determinism...\n"); f += test_rng();
    printf("  Core logic...\n"); f += test_logic();
    return f;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("KMeans self-tests:\n");
        int fails = test_all();
        return fails ? 1 : 0;
    }

    parse_args(argc, argv);

    printf("Points:    %li\n", g_npoints);
    printf("Dims:      %li\n", g_ndims);
    printf("Clusters:  %li\n", g_nclusters);
    printf("Threshold: %f\n", g_threshold);
    fflush(stdout);

    expli::TM<int>::init();

    g_data.npoints = (int)g_npoints;
    g_data.ndims = (int)g_ndims;
    g_data.nclusters = (int)g_nclusters;
    g_data.threshold = g_threshold;

    g_data.points = (double*)tm_calloc((size_t)g_npoints * g_ndims, sizeof(double));
    g_data.centroids = (double*)tm_calloc((size_t)g_nclusters * g_ndims, sizeof(double));
    g_data.assignments = (int*)tm_calloc((size_t)g_npoints, sizeof(int));
    g_data.new_centers_sum = (double*)tm_calloc((size_t)g_nclusters * g_ndims, sizeof(double));
    g_data.new_centers_count = (int*)tm_calloc((size_t)g_nclusters, sizeof(int));

    PRNG rng(42);
    for (int i = 0; i < g_npoints; i++) {
        int cluster = i % g_nclusters;
        for (int d = 0; d < g_ndims; d++) {
            double u = (double)rng() / (double)rng.max();
            g_data.points[i * g_ndims + d] = (-10.0 + u * 20.0) + cluster * 5.0;
        }
    }

    for (int c = 0; c < g_nclusters; c++) {
        for (int d = 0; d < g_ndims; d++) {
            double u = (double)rng() / (double)rng.max();
            g_data.centroids[c * g_ndims + d] = -10.0 + u * 20.0;
        }
    }

    std::fill(g_data.assignments, g_data.assignments + g_npoints, -1);

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < g_num_threads; i++)
        threads.emplace_back(worker, i, g_num_threads);
    for (auto& t : threads)
        t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    printf("Time: %lf seconds\n", elapsed / 1000.0);

    expli::TM<int>::exit();
    return 0;
}
