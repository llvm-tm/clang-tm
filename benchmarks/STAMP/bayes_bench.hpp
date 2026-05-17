#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <vector>

struct Task {
    int op;
    int from_id;
    int to_id;
    double score;
    bool operator<(const Task& o) const { return score > o.score; }
};

struct TM BayesData {
    int num_var;
    int num_record;
    double base_penalty;
    double base_log_likelihood;

    std::vector<std::vector<int>> records;
    std::vector<std::set<int>> parents;
    std::vector<std::set<int>> children;
    std::vector<double> local_ll;

    std::vector<Task> task_list;
    int total_parents;

    int global_max_edges;
    double quality_factor;
};

static BayesData* g_bayes = nullptr;
static std::mutex g_init_mutex;

static double compute_density_ll(BayesData* data, int var,
                                  const std::vector<int>& parents_vec) {
    int np = (int)parents_vec.size();
    int ncfg = 1 << np;
    std::vector<int> c0(ncfg, 0), c1(ncfg, 0);

    for (int r = 0; r < data->num_record; r++) {
        int cfg = 0;
        for (int i = 0; i < np; i++)
            cfg = (cfg << 1) | data->records[r][parents_vec[i]];
        if (data->records[r][var] == 0) c0[cfg]++; else c1[cfg]++;
    }

    double ll = 0.0;
    for (int c = 0; c < ncfg; c++) {
        int t = c0[c] + c1[c];
        if (t == 0) continue;
        double p0 = (double)c0[c] / t;
        double p1 = (double)c1[c] / t;
        double frac = (double)t / data->num_record;
        if (p0 > 0) ll += frac * p0 * std::log(p0);
        if (p1 > 0) ll += frac * p1 * std::log(p1);
    }
    return ll;
}

inline void bayes_generate_network() {
    auto data = new BayesData();
    data->num_var = g_bayes_v;
    data->num_record = g_bayes_r;
    data->base_penalty = 0.0;
    data->base_log_likelihood = 0.0;
    data->total_parents = 0;
    data->global_max_edges = g_bayes_e;
    data->quality_factor = 1.0;

    data->parents.resize(data->num_var);
    data->children.resize(data->num_var);
    data->local_ll.resize(data->num_var, 0.0);

    PRNG rng(g_bayes_s);
    data->records.resize(data->num_record, std::vector<int>(data->num_var));

    for (int v = 1; v < data->num_var; v++) {
        int max_np = std::min(g_bayes_n, v);
        int np = (max_np > 0) ? (int)(rng.next() % max_np) + 1 : 0;
        for (int p = 0; p < np && p < v; p++) {
            int parent = (int)(rng.next() % v);
            if (data->parents[v].find(parent) == data->parents[v].end()) {
                data->parents[v].insert(parent);
                data->children[parent].insert(v);
                data->total_parents++;
            }
        }
    }

    for (int r = 0; r < data->num_record; r++) {
        for (int v = 0; v < data->num_var; v++) {
            if (data->parents[v].empty()) {
                data->records[r][v] = (int)(rng.next() % 2);
            } else {
                int val = 0;
                for (int p : data->parents[v])
                    val = (val << 1) | data->records[r][p];
                int threshold = 30;
                for (int p : data->parents[v])
                    threshold += (p * 7 + v * 11) % 40;
                threshold = (threshold + val * 17) % 100;
                data->records[r][v] = (rng.next() % 100 < threshold) ? 1 : 0;
            }
        }
    }

    data->base_penalty = g_bayes_i > 0
        ? -0.5 * std::log((double)data->num_record) * g_bayes_i
        : 0.0;
    g_bayes = data;
}

static bool has_path(BayesData* data, int from, int to) {
    std::vector<bool> visited(data->num_var, false);
    std::vector<int> stack = {from};
    visited[from] = true;
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        for (int c : data->children[cur]) {
            if (c == to) return true;
            if (!visited[c]) { visited[c] = true; stack.push_back(c); }
        }
    }
    return false;
}

TX static Task pop_best_task(BayesData* data) {
    tm_serialize_lock();
    if (data->task_list.empty()) { tm_serialize_unlock(); return {-1, -1, -1, -1e100}; }
    std::pop_heap(data->task_list.begin(), data->task_list.end());
    Task t = data->task_list.back();
    data->task_list.pop_back();
    tm_serialize_unlock();
    return t;
}

TX static void insert_task(BayesData* data, const Task& t) {
    tm_serialize_lock();
    data->task_list.push_back(t);
    std::push_heap(data->task_list.begin(), data->task_list.end());
    tm_serialize_unlock();
}

TX static bool apply_insert(BayesData* data, int from, int to) {
    tm_serialize_lock();
    if (data->parents[to].find(from) != data->parents[to].end()) { tm_serialize_unlock(); return false; }
    if (has_path(data, to, from)) { tm_serialize_unlock(); return false; }
    if (data->global_max_edges > 0 && (int)data->parents[to].size() >= data->global_max_edges)
        { tm_serialize_unlock(); return false; }

    data->parents[to].insert(from);
    data->children[from].insert(to);
    data->total_parents++;

    std::vector<int> par(data->parents[to].begin(), data->parents[to].end());
    double new_ll = compute_density_ll(data, to, par);
    data->base_log_likelihood += (new_ll - data->local_ll[to]);
    data->local_ll[to] = new_ll;
    tm_serialize_unlock();

    return true;
}

TX static Task find_best_insert(BayesData* data, int to) {
    tm_serialize_lock();
    Task best = {-1, -1, -1, -1e100};
    double base_ll = data->local_ll[to];

    for (int from = 0; from < data->num_var; from++) {
        if (from == to) continue;
        if (data->parents[to].find(from) != data->parents[to].end()) continue;
        if (has_path(data, to, from)) continue;
        if (data->global_max_edges > 0 && (int)data->parents[to].size() >= data->global_max_edges)
            continue;

        std::vector<int> par(data->parents[to].begin(), data->parents[to].end());
        par.push_back(from);
        double new_ll = compute_density_ll(data, to, par);
        double delta = new_ll - base_ll;
        double score = data->total_parents * data->base_penalty +
                       data->num_record * (data->base_log_likelihood + delta);

        if (score > best.score)
            best = {0, from, to, score};
    }
    tm_serialize_unlock();
    return best;
}

static void init_worker(BayesData* data, int nvar, int start, int end) {
    for (int v = start; v < end; v++) {
        data->local_ll[v] = compute_density_ll(data, v, {});
        data->base_log_likelihood += data->local_ll[v];
    }
    for (int v = start; v < end; v++) {
        for (int from = 0; from < nvar; from++) {
            if (from == v) continue;
            double with_ll = compute_density_ll(data, v, {from});
            if (with_ll > data->local_ll[v]) {
                double delta = with_ll - data->local_ll[v];
                double score = data->base_penalty +
                               data->num_record * (data->base_log_likelihood + delta);
                insert_task(data, {0, from, v, score});
            }
        }
    }
}

THREAD void worker_bayes(ThreadData* td) {
    auto data = g_bayes;
    int nvar = data->num_var;
    int chunk = (nvar + g_num_threads - 1) / g_num_threads;
    int start = td->thread_id * chunk;
    int end = std::min(start + chunk, nvar);

    // Serialize initialization to avoid data races on non-TX code
    {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        init_worker(data, nvar, start, end);
    }

    for (;;) {
        Task t = pop_best_task(data);
        if (t.op < 0) break;

        bool ok = apply_insert(data, t.from_id, t.to_id);
        if (ok) {
            Task next = find_best_insert(data, t.to_id);
            if (next.op >= 0)
                insert_task(data, next);
            total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
