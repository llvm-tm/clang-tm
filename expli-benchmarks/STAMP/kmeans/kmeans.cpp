// STAMP/kmeans benchmark — explicit TM API port
// Matches the plugin kmeans_bench.hpp algorithm.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
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

using PRNG = std::mt19937_64;

// ── TM-safe double read/write helpers ─────────────────────
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

// ── KMeans data ───────────────────────────────────────────
struct KMeansData {
    double* points;         // [npoints * ndims] row-major
    double* centroids;      // [nclusters * ndims] row-major
    int*    assignments;    // [npoints]
    double* new_centers_sum;
    int*    new_centers_count;
    int npoints;
    int ndims;
    int nclusters;
    double threshold;
};

static KMeansData g_data;

static double tm_sqrt_s(double x) {
    if (x <= 0) return 0;
    double s = x;
    for (int i = 0; i < 25; i++) s = (s + x / s) * 0.5;
    return s;
}

static void kmeans_accumulate(KMeansData* data, int start, int end,
                               double* local_sum, int* local_count) {
    int ndims = data->ndims;
    int nclusters = data->nclusters;

    for (int i = start; i < end; i++) {
        int best = -1;
        double best_dist = std::numeric_limits<double>::max();

        // One TX per point: read centroids, compute assignment, write assignment
        double pt_buf[64];
        double ct_buf[64 * 64];
        tx_retry([&]() {
            for (int d = 0; d < ndims; d++)
                pt_buf[d] = tm_read_double(&data->points[i * ndims + d]);
            for (int c = 0; c < nclusters; c++)
                for (int d = 0; d < ndims; d++)
                    ct_buf[c * ndims + d] = tm_read_double(&data->centroids[c * ndims + d]);

            best = -1;
            best_dist = std::numeric_limits<double>::max();
            for (int c = 0; c < nclusters; c++) {
                double dist = 0.0;
                for (int d = 0; d < ndims; d++) {
                    double diff = pt_buf[d] - ct_buf[c * ndims + d];
                    dist += diff * diff;
                }
                if (dist < best_dist) { best_dist = dist; best = c; }
            }
            tm_write_int(&data->assignments[i], best);
        });

        local_count[best]++;
        for (int d = 0; d < ndims; d++)
            local_sum[best * ndims + d] += pt_buf[d];
    }
}

static void worker(int thread_id, int num_threads) {
    expli::TM<int>::thread_init();

    auto* data = &g_data;
    int npoints = data->npoints;
    int ndims = data->ndims;
    int nclusters = data->nclusters;
    double threshold = data->threshold;

    auto* local_sum = new double[nclusters * ndims]();
    auto* local_count = new int[nclusters]();

    bool converged = false;
    int max_iters = 100;

    while (!converged && max_iters-- > 0) {
        std::fill(local_sum, local_sum + nclusters * ndims, 0.0);
        std::fill(local_count, local_count + nclusters, 0);

        int chunk = (npoints + num_threads - 1) / num_threads;
        int start = thread_id * chunk;
        int end = std::min(start + chunk, npoints);

        if (start < end) {
            kmeans_accumulate(data, start, end, local_sum, local_count);
        }

        // Serial accumulate (no TM — matches plugin behavior)
        for (int c = 0; c < nclusters; c++) {
            data->new_centers_count[c] += local_count[c];
            for (int d = 0; d < ndims; d++) {
                data->new_centers_sum[c * ndims + d] += local_sum[c * ndims + d];
            }
        }

        double delta = 0.0;
        for (int c = 0; c < nclusters; c++) {
            if (data->new_centers_count[c] > 0) {
                for (int d = 0; d < ndims; d++) {
                    int idx = c * ndims + d;
                    double new_val = data->new_centers_sum[idx] / data->new_centers_count[c];
                    double diff = data->centroids[idx] - new_val;
                    delta += diff * diff;
                    data->centroids[idx] = new_val;
                    data->new_centers_sum[idx] = 0.0;
                }
            }
            data->new_centers_count[c] = 0;
        }

        delta = tm_sqrt_s(delta / (nclusters * ndims));
        converged = (delta < threshold);
    }

    delete[] local_sum;
    delete[] local_count;
    expli::TM<int>::thread_exit();
}

int main(int argc, char* argv[]) {
    int num_threads = 4;
    int nclusters = 16;
    int ndims = 2;
    int npoints = 2048;
    double threshold = 0.0001;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) nclusters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) ndims = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) npoints = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) threshold = atof(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -k <clusters> -d <dims> -n <points> -t <threshold>\n", argv[0]);
            return 0;
        }
    }

    printf("Points:    %i\n", npoints);
    printf("Dims:      %i\n", ndims);
    printf("Clusters:  %i\n", nclusters);
    printf("Threshold: %f\n", threshold);
    fflush(stdout);

    expli::TM<int>::init();

    g_data.npoints = npoints;
    g_data.ndims = ndims;
    g_data.nclusters = nclusters;
    g_data.threshold = threshold;
    g_data.points = (double*)tm_calloc((size_t)npoints * ndims, sizeof(double));
    g_data.centroids = (double*)tm_calloc((size_t)nclusters * ndims, sizeof(double));
    g_data.assignments = (int*)tm_calloc(npoints, sizeof(int));
    g_data.new_centers_sum = (double*)tm_calloc((size_t)nclusters * ndims, sizeof(double));
    g_data.new_centers_count = (int*)tm_calloc((size_t)nclusters, sizeof(int));

    PRNG rng(42);
    for (int i = 0; i < npoints; i++) {
        int cluster = i % nclusters;
        for (int d = 0; d < ndims; d++) {
            double u = (double)rng() / (double)rng.max();
            g_data.points[i * ndims + d] = (-10.0 + u * 20.0) + cluster * 5.0;
        }
    }

    for (int c = 0; c < nclusters; c++) {
        for (int d = 0; d < ndims; d++) {
            double u = (double)rng() / (double)rng.max();
            g_data.centroids[c * ndims + d] = -10.0 + u * 20.0;
        }
    }

    std::fill(g_data.assignments, g_data.assignments + npoints, -1);

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++)
        threads.emplace_back(worker, i, num_threads);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    printf("    Elapsed = %lld ms\n", (long long)elapsed);

    expli::TM<int>::exit();
    return 0;
}
