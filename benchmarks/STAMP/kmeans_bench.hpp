#pragma once

#include "stamp_common.hpp"
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

struct TM KMeansData {
    std::vector<std::vector<double>> points;
    std::vector<std::vector<double>> centroids;
    std::vector<double> new_centers_sum;
    std::vector<int> new_centers_count;
    std::vector<int> assignments;
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
    data->points.resize(npoints, std::vector<double>(ndims));
    data->centroids.resize(nclusters, std::vector<double>(ndims));
    data->new_centers_sum.resize(nclusters * ndims, 0.0);
    data->new_centers_count.resize(nclusters, 0);
    data->assignments.resize(npoints, -1);

    PRNG rng(42);
    for (int i = 0; i < npoints; i++) {
        int cluster = i % nclusters;
        for (int d = 0; d < ndims; d++) {
            data->points[i][d] = rng.uniform(-10.0, 10.0) + cluster * 5.0;
        }
    }

    for (int c = 0; c < nclusters; c++) {
        for (int d = 0; d < ndims; d++) {
            data->centroids[c][d] = rng.uniform(-10.0, 10.0);
        }
    }

    g_kmeans = data;
}

static inline int find_nearest_cluster(const std::vector<double>& point,
                                        const std::vector<std::vector<double>>& centroids,
                                        int nclusters) {
    int best = -1;
    double best_dist = std::numeric_limits<double>::max();
    for (int c = 0; c < nclusters; c++) {
        double dist = 0.0;
        for (size_t d = 0; d < point.size(); d++) {
            double diff = point[d] - centroids[c][d];
            dist += diff * diff;
        }
        if (dist < best_dist) {
            best_dist = dist;
            best = c;
        }
    }
    return best;
}

TX static void kmeans_accumulate(KMeansData* data, int start, int end,
                                   std::vector<double>& local_sum,
                                   std::vector<int>& local_count) {
    for (int i = start; i < end; i++) {
        int c = find_nearest_cluster(data->points[i], data->centroids, data->nclusters);
        data->assignments[i] = c;
        local_count[c]++;
        for (int d = 0; d < data->ndims; d++) {
            local_sum[c * data->ndims + d] += data->points[i][d];
        }
    }
}

THREAD void worker_kmeans(ThreadData* td) {
    auto data = g_kmeans;
    auto& points = data->points;
    auto& centroids = data->centroids;
    auto& assignments = data->assignments;

    int npoints = data->npoints;
    int ndims = data->ndims;
    int nclusters = data->nclusters;
    double threshold = data->threshold;
    int total_threads = g_num_threads;
    int tid = td->thread_id;

    std::vector<double> local_sum(nclusters * ndims, 0.0);
    std::vector<int> local_count(nclusters, 0);

    bool converged = false;
    int max_iters = 100;

    while (!converged && max_iters-- > 0) {
        std::fill(local_sum.begin(), local_sum.end(), 0.0);
        std::fill(local_count.begin(), local_count.end(), 0);

        int chunk = (npoints + total_threads - 1) / total_threads;
        int start = tid * chunk;
        int end = std::min(start + chunk, npoints);

        if (start < end) {
            kmeans_accumulate(data, start, end, local_sum, local_count);
        }

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
                    double new_val = data->new_centers_sum[c * ndims + d] / data->new_centers_count[c];
                    double diff = centroids[c][d] - new_val;
                    delta += diff * diff;
                    centroids[c][d] = new_val;
                    data->new_centers_sum[c * ndims + d] = 0.0;
                }
            }
            data->new_centers_count[c] = 0;
        }

        delta = std::sqrt(delta / (nclusters * ndims));
        converged = (delta < threshold);
        total_ops.fetch_add(npoints, std::memory_order_relaxed);
    }
}
