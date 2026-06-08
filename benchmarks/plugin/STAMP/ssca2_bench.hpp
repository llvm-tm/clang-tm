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
    uint64_t* row_ptr;
    uint64_t* col_idx;
    int64_t* weights;
    uint64_t row_count;
    uint64_t col_count;
};

struct TM SSCA2Data {
    Edge* edges;
    uint64_t num_edges_alloc;
    SparseRow graph;
    uint64_t num_vertices;
    uint64_t num_edges;
    int scale;
    int max_paral_edges;
    double perc_int_weights;
    double prob_unidirectional;
    double prob_intercl_edges;
    int subgr_edge_length;
    int64_t global_max_weight;
    int64_t* tri_count;
};

static SSCA2Data* g_ssca2 = nullptr;

static void build_csr(SSCA2Data* data) {
    Edge* edges = data->edges;
    uint64_t ne = data->num_edges;
    if (ne == 0) return;

    std::sort(edges, edges + ne,
              [](const Edge& a, const Edge& b) {
                  if (a.src != b.src) return a.src < b.src;
                  return a.dst < b.dst;
              });

    // Unique
    uint64_t write_idx = 0;
    for (uint64_t i = 0; i < ne; i++) {
        if (i == 0 || edges[i].src != edges[i-1].src || edges[i].dst != edges[i-1].dst) {
            edges[write_idx++] = edges[i];
        }
    }
    data->num_edges = write_idx;
    ne = write_idx;

    uint64_t max_v = 0;
    for (uint64_t i = 0; i < ne; i++) {
        max_v = std::max(max_v, std::max(edges[i].src, edges[i].dst));
    }
    data->num_vertices = max_v + 1;

    auto& g = data->graph;
    g.row_count = data->num_vertices + 1;
    g.row_ptr = new uint64_t[g.row_count]();
    g.col_count = ne;
    g.col_idx = new uint64_t[ne]();
    g.weights = new int64_t[ne]();

    for (uint64_t i = 0; i < ne; i++) {
        g.row_ptr[edges[i].src + 1]++;
    }

    for (uint64_t i = 1; i <= data->num_vertices; i++) {
        g.row_ptr[i] += g.row_ptr[i - 1];
    }

    uint64_t* temp_pos = new uint64_t[g.row_count];
    for (uint64_t i = 0; i < g.row_count; i++) temp_pos[i] = g.row_ptr[i];

    for (uint64_t i = 0; i < ne; i++) {
        uint64_t pos = temp_pos[edges[i].src]++;
        g.col_idx[pos] = edges[i].dst;
        g.weights[pos] = edges[i].weight;
    }
    delete[] temp_pos;
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

    data->num_edges = (uint64_t)temp_edges.size();
    data->num_edges_alloc = data->num_edges + 1024;
    data->edges = new Edge[data->num_edges_alloc]();
    for (uint64_t i = 0; i < data->num_edges; i++) {
        data->edges[i] = temp_edges[i];
    }

    build_csr(data);
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

static bool ssca2_has_edge(const SSCA2Data* data, uint64_t src, uint64_t dst) {
    uint64_t start = data->graph.row_ptr[src];
    uint64_t end = data->graph.row_ptr[src + 1];
    for (uint64_t i = start; i < end; i++) {
        if (data->graph.col_idx[i] == dst) return true;
    }
    return false;
}

static int64_t ssca2_get_weight(const SSCA2Data* data, uint64_t src, uint64_t dst) {
    uint64_t start = data->graph.row_ptr[src];
    uint64_t end = data->graph.row_ptr[src + 1];
    for (uint64_t i = start; i < end; i++) {
        if (data->graph.col_idx[i] == dst) return data->graph.weights[i];
    }
    return -1;
}



TX static void ssca2_update_max_weight(SSCA2Data* data, int64_t local_max,
                                        uint64_t local_ops,
                                        uint64_t* ops_out) {
    if (local_max > data->global_max_weight)
        data->global_max_weight = local_max;
    *ops_out = local_ops;
}

THREAD void worker_ssca2(ThreadData* td) {
    auto data = g_ssca2;

    uint64_t chunk = (data->num_vertices + g_num_threads - 1) / g_num_threads;
    uint64_t start_v = td->thread_id * chunk;
    uint64_t end_v = std::min(start_v + chunk, data->num_vertices);

    int64_t local_max = 0;
    uint64_t local_ops = 0;

    // ── Non-TX: iterate vertices, read graph data, count triangles ──
    for (int iter = 0; iter < g_ssca2_i; iter++) {
        for (uint64_t v = start_v; v < end_v; v++) {
            uint64_t start = data->graph.row_ptr[v];
            uint64_t end = data->graph.row_ptr[v + 1];

            for (uint64_t i = start; i < end; i++) {
                int64_t w = data->graph.weights[i];
                if (w > local_max) local_max = w;

                uint64_t neighbor = data->graph.col_idx[i];
                uint64_t nstart = data->graph.row_ptr[neighbor];
                uint64_t nend = data->graph.row_ptr[neighbor + 1];

                for (uint64_t j = nstart; j < nend; j++) {
                    uint64_t n2 = data->graph.col_idx[j];
                    if (ssca2_has_edge(data, n2, v)) local_ops++;
                }
            }
        }
    }

    // ── TX: update global max weight ──
    uint64_t ops = 0;
    ssca2_update_max_weight(data, local_max, local_ops, &ops);
    total_ops.fetch_add(ops, std::memory_order_relaxed);
}
