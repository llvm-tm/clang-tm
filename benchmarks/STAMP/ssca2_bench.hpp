#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <set>
#include <vector>

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

struct TM SSCA2Data {
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

static SSCA2Data* g_ssca2 = nullptr;

static void build_csr(SSCA2Data* data) {
    auto& edges = data->edges;
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
    for (auto& e : edges) {
        max_v = std::max(max_v, std::max(e.src, e.dst));
    }
    data->num_vertices = max_v + 1;

    auto& row_ptr = data->graph.row_ptr;
    auto& col_idx = data->graph.col_idx;
    auto& weights = data->graph.weights;

    row_ptr.resize(data->num_vertices + 1, 0);

    for (auto& e : edges) {
        row_ptr[e.src + 1]++;
    }

    for (uint64_t i = 1; i <= data->num_vertices; i++) {
        row_ptr[i] += row_ptr[i - 1];
    }

    col_idx.resize(edges.size());
    weights.resize(edges.size());

    std::vector<uint64_t> temp_pos = row_ptr;
    for (auto& e : edges) {
        uint64_t pos = temp_pos[e.src]++;
        col_idx[pos] = e.dst;
        weights[pos] = e.weight;
    }
}

inline void ssca2_generate_graph() {
    auto data = new SSCA2Data();
    data->scale = g_ssca2_s;
    data->max_paral_edges = g_ssca2_p;
    data->perc_int_weights = 0.6;
    data->prob_unidirectional = g_ssca2_u;
    data->prob_intercl_edges = 0.5;
    data->subgr_edge_length = g_ssca2_l;

    uint64_t tot_vertices = (uint64_t)1 << data->scale;
    int max_clique_size = 1 << (data->scale / 3);

    PRNG rng(42);

    std::vector<int> perm(tot_vertices);
    for (uint64_t i = 0; i < tot_vertices; i++) perm[i] = i;
    for (uint64_t i = tot_vertices - 1; i > 0; i--) {
        uint64_t j = rng.next() % (i + 1);
        std::swap(perm[i], perm[j]);
    }

    std::vector<int> clique_sizes;
    uint64_t assigned = 0;
    while (assigned < tot_vertices) {
        int sz = (int)(rng.next() % max_clique_size) + 1;
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
                if (rng.uniform() >= data->prob_unidirectional) {
                    uint64_t si = perm[start_v + i];
                    uint64_t sj = perm[start_v + j];
                    auto p = std::make_pair(si, sj);
                    if (edge_set.insert(p).second) {
                        int64_t w = (rng.uniform() < data->perc_int_weights)
                            ? (int64_t)(rng.next() % (1 << data->scale))
                            : -(int64_t)(rng.next() % data->scale);
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
            uint64_t v = perm[start_v + i];
            for (int d = 1; d < (int)tot_vertices; d *= 2) {
                if (rng.uniform() < data->prob_intercl_edges / (std::log2(d + 1) + 1.0)) {
                    uint64_t neighbor = (v + d) % tot_vertices;
                    for (int p = 0; p < data->max_paral_edges; p++) {
                        if (rng.uniform() < 0.5) {
                            auto p = std::make_pair(v, neighbor);
                            if (edge_set.insert(p).second) {
                                int64_t w = (rng.uniform() < data->perc_int_weights)
                                    ? (int64_t)(rng.next() % (1 << data->scale))
                                    : -(int64_t)(rng.next() % data->scale);
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
        e.src = perm[e.src % tot_vertices];
        e.dst = perm[e.dst % tot_vertices];
        data->edges.push_back(e);
    }

    build_csr(data);
    data->num_edges = data->edges.size();
    g_ssca2 = data;

    printf("Number of processors:       %i\n", g_num_threads);
    printf("Problem Scale:              %i\n", data->scale);
    printf("Max parallel edges:         %i\n", data->max_paral_edges);
    printf("Percent int weights:        %f\n", data->perc_int_weights);
    printf("Probability unidirectional: %f\n", data->prob_unidirectional);
    printf("Vertices: %lu  Edges: %lu\n",
           (unsigned long)data->num_vertices, (unsigned long)data->num_edges);
    fflush(stdout);
}

TX static bool ssca2_has_edge(SSCA2Data* data, uint64_t src, uint64_t dst) {
    auto& row_ptr = data->graph.row_ptr;
    auto& col_idx = data->graph.col_idx;
    uint64_t start = row_ptr[src];
    uint64_t end = row_ptr[src + 1];
    for (uint64_t i = start; i < end; i++) {
        if (col_idx[i] == dst) return true;
    }
    return false;
}

TX static int64_t ssca2_get_weight(SSCA2Data* data, uint64_t src, uint64_t dst) {
    auto& row_ptr = data->graph.row_ptr;
    auto& col_idx = data->graph.col_idx;
    uint64_t start = row_ptr[src];
    uint64_t end = row_ptr[src + 1];
    for (uint64_t i = start; i < end; i++) {
        if (col_idx[i] == dst) return data->graph.weights[i];
    }
    return -1;
}

THREAD void worker_ssca2(ThreadData* td) {
    auto data = g_ssca2;
    auto& row_ptr = data->graph.row_ptr;
    auto& col_idx = data->graph.col_idx;

    uint64_t chunk = (data->num_vertices + g_num_threads - 1) / g_num_threads;
    uint64_t start_v = td->thread_id * chunk;
    uint64_t end_v = std::min(start_v + chunk, data->num_vertices);
    uint64_t local_ops = 0;

    for (int iter = 0; iter < g_ssca2_i; iter++) {
        for (uint64_t v = start_v; v < end_v; v++) {
            uint64_t start = row_ptr[v];
            uint64_t end = row_ptr[v + 1];

            for (uint64_t i = start; i < end; i++) {
                uint64_t neighbor = col_idx[i];
                uint64_t nstart = row_ptr[neighbor];
                uint64_t nend = row_ptr[neighbor + 1];

                for (uint64_t j = nstart; j < nend; j++) {
                    uint64_t n2 = col_idx[j];
                    bool triangle = ssca2_has_edge(data, n2, v);
                    if (triangle) {
                        local_ops++;
                    }
                }
            }
        }
    }

    total_ops.fetch_add(local_ops, std::memory_order_relaxed);
}
