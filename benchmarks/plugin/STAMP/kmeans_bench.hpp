#pragma once

#include "stamp_common.hpp"
#include <cstring>
#include <limits>
#include <vector>

struct TM KMeansData {
    double* points;
    double* centroids;
    double* new_centers_sum;
    int* new_centers_count;
    int* assignments;
    int npoints;
    int ndims;
    int nclusters;
    double threshold;
};

static KMeansData* g_kmeans = nullptr;

inline void kmeans_generate_points() {
    int npoints = g_kmeans_n;
    int ndims = g_kmeans_m;
    int nclusters = g_kmeans_m;

    auto data = new KMeansData();
    data->npoints = npoints;
    data->ndims = ndims;
    data->nclusters = nclusters;
    data->threshold = g_kmeans_t;

    data->points = new double[npoints * ndims]();
    data->centroids = new double[nclusters * ndims]();
    data->new_centers_sum = new double[nclusters * ndims]();
    data->new_centers_count = new int[nclusters]();
    data->assignments = new int[npoints]();

    PRNG rng(42);
    for (int i = 0; i < npoints; i++) {
        int cluster = i % nclusters;
        for (int d = 0; d < ndims; d++) {
            data->points[i * ndims + d] = rng.uniform(-10.0, 10.0) + cluster * 5.0;
        }
    }

    for (int c = 0; c < nclusters; c++) {
        for (int d = 0; d < ndims; d++) {
            data->centroids[c * ndims + d] = rng.uniform(-10.0, 10.0);
        }
    }

    g_kmeans = data;

    printf("Points:    %i\n", npoints);
    printf("Dims:      %i\n", ndims);
    printf("Clusters:  %i\n", nclusters);
    printf("Threshold: %f\n", data->threshold);
    fflush(stdout);
}

static inline int find_nearest_cluster(const double* point, const double* centroids,
                                        int nclusters, int ndims) {
    int best = -1;
    double best_dist = std::numeric_limits<double>::max();
    for (int c = 0; c < nclusters; c++) {
        double dist = 0.0;
        for (int d = 0; d < ndims; d++) {
            double diff = point[d] - centroids[c * ndims + d];
            dist += diff * diff;
        }
        if (dist < best_dist) {
            best_dist = dist;
            best = c;
        }
    }
    return best;
}

TX static void kmeans_merge_global(KMeansData* data,
                                    double* local_sum, int* local_count) {
    for (int c = 0; c < data->nclusters; c++) {
        int lc = local_count[c];
        if (lc > 0) {
            data->new_centers_count[c] += lc;
            for (int d = 0; d < data->ndims; d++) {
                data->new_centers_sum[c * data->ndims + d] += local_sum[c * data->ndims + d];
            }
        }
    }
}

static void kmeans_accum_local(double* local_sum, int* local_count,
                                KMeansData* data, int start, int end) {
    for (int i = start; i < end; i++) {
        int c = find_nearest_cluster(&data->points[i * data->ndims],
                                      data->centroids, data->nclusters, data->ndims);
        local_count[c]++;
        for (int d = 0; d < data->ndims; d++) {
            local_sum[c * data->ndims + d] += data->points[i * data->ndims + d];
        }
    }
}

TX static double kmeans_update_one_cluster(KMeansData* data, int c) {
    if (data->new_centers_count[c] == 0) return 0.0;

    double delta = 0.0;
    for (int d = 0; d < data->ndims; d++) {
        double new_val = data->new_centers_sum[c * data->ndims + d] / data->new_centers_count[c];
        double diff = data->centroids[c * data->ndims + d] - new_val;
        delta += diff * diff;
        data->centroids[c * data->ndims + d] = new_val;
        data->new_centers_sum[c * data->ndims + d] = 0.0;
    }
    data->new_centers_count[c] = 0;
    return delta;
}

THREAD void worker_kmeans(ThreadData* td) {
    auto data = g_kmeans;

    int npoints = data->npoints;
    int ndims = data->ndims;
    int nclusters = data->nclusters;
    double threshold = data->threshold;
    int total_threads = g_num_threads;
    int tid = td->thread_id;

    double* local_sum = new double[nclusters * ndims]();
    int* local_count = new int[nclusters]();

    bool converged = false;
    int max_iters = 100;

    while (!converged && max_iters-- > 0) {
        for (int i = 0; i < nclusters * ndims; i++) local_sum[i] = 0.0;
        for (int i = 0; i < nclusters; i++) local_count[i] = 0;

        // Phase 1: Non-TX accumulation into thread-local arrays
        int chunk = (npoints + total_threads - 1) / total_threads;
        int start = tid * chunk;
        int end = std::min(start + chunk, npoints);

        if (start < end) {
            kmeans_accum_local(local_sum, local_count, data, start, end);
        }

        // Phase 2: TX merge — each thread atomically adds its local counts/sums
        // to the global accumulators
        kmeans_merge_global(data, local_sum, local_count);

        // Phase 3: Thread 0 updates centroids from accumulators (one TX per cluster)
        if (tid == 0) {
            double delta = 0.0;
            for (int c = 0; c < nclusters; c++) {
                delta += kmeans_update_one_cluster(data, c);
            }
            converged = (delta < threshold);
        }

        total_ops.fetch_add(npoints, std::memory_order_relaxed);
    }

    delete[] local_sum;
    delete[] local_count;
}
