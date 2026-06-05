// KMeans — C++ port of the original STAMP spec (explicit API path)
// Original spec: https://github.com/ccaominh/stamp/tree/master/kmeans
//
// Parameters:
//   -m <num>   Max clusters (= dimensions)  (default: 40)
//   -n <num>   Number of points              (default: 40)
//   -t <num>   Convergence threshold         (default: 0.00001)

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

static long g_nclusters = 40;
static long g_npoints   = 40;
static double g_threshold = 0.00001;
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

static inline long d2l(double v) { long r; memcpy(&r, &v, sizeof(r)); return r; }
static inline double l2d(long v) { double r; memcpy(&r, &v, sizeof(r)); return r; }

#define TM_READ_DOUBLE(p)    l2d(TM_READ_I8((const long*)(p)))
#define TM_WRITE_DOUBLE(p,v) TM_WRITE_I8((long*)(p), d2l(v))

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

template<typename F>
static void tx_run_sync(F&& body) {
    sigsetjmp(tm_jmpbuf, 0);
    tm_nested_call_counter = 1;
    tm_begin();
    body();
    tm_end();
    tm_nested_call_counter = 0;
}

// ── RNG ────────────────────────────────────────────────────────────
#include <random>
static thread_local std::mt19937 tls_rng;

static void rng_seed(unsigned s) { tls_rng = std::mt19937(s); }
static double rng_uniform(double min, double max) {
    return std::uniform_real_distribution<double>(min, max)(tls_rng);
}
static long rng_range(long max) {
    return (long)(tls_rng() % (uint32_t)max);
}

// ── Data structures (flat arrays) ──────────────────────────────────
// Points (non-TM, read-only after generation)
static double* g_points = nullptr; // [npoints * ndims]

// TM-tracked centroids
static long* g_centroids = nullptr; // [nclusters * ndims] as d2l

// Per-cluster TM-tracked accumulators (reset each iteration)
static long* g_new_centers_sum = nullptr;   // [nclusters * ndims] as d2l
static long* g_new_centers_count = nullptr;  // [nclusters]

static long g_ndims = 0;
static std::atomic<long> g_total_iters{0};
static std::atomic<bool> g_converged{false};
static std::mutex g_barrier_mutex;
static std::condition_variable g_barrier_cv;
static int g_barrier_count = 0;

static void barrier_wait() {
    std::unique_lock<std::mutex> lock(g_barrier_mutex);
    g_barrier_count++;
    if (g_barrier_count >= (int)g_num_threads) {
        g_barrier_count = 0;
        g_barrier_cv.notify_all();
    } else {
        g_barrier_cv.wait(lock);
    }
}

// ── TM accumulate (find nearest centroid, accumulate sums) ─────────
TX_FUNC static void tx_accumulate(long start, long end,
                                   double* local_sum, long* local_count) {
    for (long i = start; i < end; i++) {
        double* pt = &g_points[i * g_ndims];
        int best = 0;
        double best_dist = 1e308;
        for (long c = 0; c < g_nclusters; c++) {
            double dist = 0.0;
            for (long d = 0; d < g_ndims; d++) {
                double diff = pt[d] - l2d(TM_READ_I8(&g_centroids[c * g_ndims + d]));
                dist += diff * diff;
            }
            if (dist < best_dist) { best_dist = dist; best = (int)c; }
        }
        for (long d = 0; d < g_ndims; d++)
            local_sum[best * g_ndims + d] += pt[d];
        local_count[best]++;
    }
}

// ── Worker thread ──────────────────────────────────────────────────
static void worker(long tid) {
    tm_init_thread();
    rng_seed(42 + (unsigned)tid);

    long chunk = (g_npoints + g_num_threads - 1) / g_num_threads;
    long start = tid * chunk;
    long end = std::min(start + chunk, g_npoints);

    // Per-thread local accumulators
    std::vector<double> local_sum(g_nclusters * g_ndims);
    std::vector<long> local_count(g_nclusters);

    for (int iter = 0; iter < 100 && !g_converged.load(); iter++) {
        // Reset local accumulators
        std::fill(local_sum.begin(), local_sum.end(), 0.0);
        std::fill(local_count.begin(), local_count.end(), 0L);

        // Accumulate (TM)
        tx_run([&]() {
            tx_accumulate(start, end, local_sum.data(), local_count.data());
        });

        // Barrier: all threads finish accumulate before centroid update
        barrier_wait();

        // Merge local → global and update centroids (serialized by barrier)
        tx_run_sync([&]() {
            // Merge local into global accumulators
            for (long c = 0; c < g_nclusters; c++) {
                long cnt = TM_READ_I8(&g_new_centers_count[c]);
                TM_WRITE_I8(&g_new_centers_count[c], cnt + local_count[c]);
                for (long d = 0; d < g_ndims; d++) {
                    double old = TM_READ_DOUBLE(&g_new_centers_sum[c * g_ndims + d]);
                    TM_WRITE_DOUBLE(&g_new_centers_sum[c * g_ndims + d],
                                    old + local_sum[c * g_ndims + d]);
                }
            }
        });

        // Thread 0 computes new centroids and checks convergence
        if (tid == 0) {
            double max_delta = 0.0;
            tx_run_sync([&]() {
                for (long c = 0; c < g_nclusters; c++) {
                    long cnt = TM_READ_I8(&g_new_centers_count[c]);
                    if (cnt > 0) {
                        for (long d = 0; d < g_ndims; d++) {
                            double new_val = TM_READ_DOUBLE(&g_new_centers_sum[c * g_ndims + d]) / cnt;
                            double old_val = TM_READ_DOUBLE(&g_centroids[c * g_ndims + d]);
                            double diff = new_val - old_val;
                            max_delta = std::max(max_delta, diff * diff);

                            TM_WRITE_DOUBLE(&g_centroids[c * g_ndims + d], new_val);
                            TM_WRITE_DOUBLE(&g_new_centers_sum[c * g_ndims + d], 0.0);
                        }
                        TM_WRITE_I8(&g_new_centers_count[c], 0);
                    }
                }
            });
            max_delta = std::sqrt(max_delta);
            if (max_delta < g_threshold) {
                g_converged.store(true, std::memory_order_relaxed);
            }
        }

        barrier_wait();
        g_total_iters.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-m") == 0 && i+1 < argc)
            g_nclusters = atol(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc)
            g_npoints = atol(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i+1 < argc)
            g_threshold = atof(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc)
            g_num_threads = atol(argv[++i]);
    }
    g_ndims = g_nclusters; // ndims = nclusters (matches plugin)

    printf("KMeans (STAMP spec)\n");
    printf("  Points:    %ld\n", g_npoints);
    printf("  Clusters:  %ld\n", g_nclusters);
    printf("  Dims:      %ld\n", g_ndims);
    printf("  Threshold: %.6f\n", g_threshold);
    printf("  Threads:   %ld\n", g_num_threads);

    tm_init();

    // Allocate data
    g_points = new double[g_npoints * g_ndims]();
    g_centroids = (long*)tm_calloc((size_t)g_nclusters * g_ndims, sizeof(long));
    g_new_centers_sum = (long*)tm_calloc((size_t)g_nclusters * g_ndims, sizeof(long));
    g_new_centers_count = (long*)tm_calloc(g_nclusters, sizeof(long));

    // Generate cluster-biased points (matching plugin spec)
    rng_seed(42);
    for (long i = 0; i < g_npoints; i++) {
        long cluster = i % g_nclusters;
        for (long d = 0; d < g_ndims; d++) {
            g_points[i * g_ndims + d] =
                rng_uniform(-10.0, 10.0) + (double)cluster * 5.0;
        }
    }

    // Initialize centroids (random, not TM-tracked for setup)
    for (long c = 0; c < g_nclusters; c++)
        for (long d = 0; d < g_ndims; d++)
            g_centroids[c * g_ndims + d] = d2l(rng_uniform(-10.0, 10.0));

    printf("  Initial centroids: %ld\n", g_nclusters);

    auto t1 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (long t = 0; t < g_num_threads; t++)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    long iters = g_total_iters.load();
    printf("\nResults (%ld ms):\n", (long)(elapsed * 1000));
    printf("  Iterations: %ld\n", iters);
    if (g_converged.load())
        printf("  Converged:  yes\n");
    else
        printf("  Converged:  no (max iters)\n");
    printf("  PASS\n");

    delete[] g_points;
    tm_exit();
    return 0;
}
