#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#define BAYES_MAX_PARENTS 64

struct Task {
    int op;
    int from_id;
    int to_id;
    double score;
};

struct TM BayesData {
    int* records;
    int num_var;
    int num_record;
    int* parents;
    int* parents_cnt;
    int* children_adj;
    int* children_cnt;
    double* local_ll;
    Task* task_list;
    int task_count;
    int task_capacity;
    double base_penalty;
    double base_log_likelihood;
    int total_parents;
    int global_max_edges;
    double quality_factor;
};

static BayesData* g_bayes = nullptr;
static std::mutex g_init_mutex;

static inline bool task_heap_empty(int count) { return count == 0; }

static inline void task_heap_push(Task* heap, int& count, const Task& t) {
    int i = count++;
    heap[i] = t;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].score >= heap[i].score) break;
        Task tmp = heap[p]; heap[p] = heap[i]; heap[i] = tmp;
        i = p;
    }
}

static inline Task task_heap_pop(Task* heap, int& count) {
    Task top = heap[0];
    heap[0] = heap[--count];
    int i = 0;
    while (true) {
        int largest = i;
        int l = 2 * i + 1;
        int r = 2 * i + 2;
        if (l < count && heap[l].score > heap[largest].score) largest = l;
        if (r < count && heap[r].score > heap[largest].score) largest = r;
        if (largest == i) break;
        Task tmp = heap[i]; heap[i] = heap[largest]; heap[largest] = tmp;
        i = largest;
    }
    return top;
}

TX static double compute_density_ll(BayesData* data, int var,
                                  const int* parents_vec, int np) {
    int ncfg = 1 << np;
    int c0[128], c1[128];
    for (int i = 0; i < ncfg; i++) c0[i] = c1[i] = 0;

    for (int r = 0; r < data->num_record; r++) {
        int cfg = 0;
        for (int i = 0; i < np; i++)
            cfg = (cfg << 1) | data->records[r * data->num_var + parents_vec[i]];
        if (data->records[r * data->num_var + var] == 0) c0[cfg]++; else c1[cfg]++;
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

    int nvar = data->num_var;
    int nrec = data->num_record;

    data->records = new int[nrec * nvar]();
    data->parents = new int[nvar * BAYES_MAX_PARENTS]();
    data->parents_cnt = new int[nvar]();
    data->children_adj = new int[nvar * BAYES_MAX_PARENTS]();
    data->children_cnt = new int[nvar]();
    data->local_ll = new double[nvar]();

    int task_cap = nvar * nvar;
    data->task_list = new Task[task_cap]();
    data->task_count = 0;
    data->task_capacity = task_cap;

    PRNG rng(g_bayes_s);

    for (int v = 1; v < nvar; v++) {
        int max_np = std::min(g_bayes_n, v);
        int np = (max_np > 0) ? (int)(rng.next() % max_np) + 1 : 0;
        for (int p = 0; p < np && p < v; p++) {
            int parent = (int)(rng.next() % v);
            int pc = data->parents_cnt[v];
            bool found = false;
            for (int i = 0; i < pc; i++) {
                if (data->parents[v * BAYES_MAX_PARENTS + i] == parent) {
                    found = true; break;
                }
            }
            if (!found && pc < BAYES_MAX_PARENTS) {
                data->parents[v * BAYES_MAX_PARENTS + pc] = parent;
                data->parents_cnt[v] = pc + 1;
                int cc = data->children_cnt[parent];
                data->children_adj[parent * BAYES_MAX_PARENTS + cc] = v;
                data->children_cnt[parent] = cc + 1;
                data->total_parents++;
            }
        }
    }

    for (int r = 0; r < nrec; r++) {
        for (int v = 0; v < nvar; v++) {
            if (data->parents_cnt[v] == 0) {
                data->records[r * nvar + v] = (int)(rng.next() % 2);
            } else {
                int val = 0;
                int* pv = &data->parents[v * BAYES_MAX_PARENTS];
                for (int i = 0; i < data->parents_cnt[v]; i++)
                    val = (val << 1) | data->records[r * nvar + pv[i]];
                int threshold = 30;
                for (int i = 0; i < data->parents_cnt[v]; i++)
                    threshold += (pv[i] * 7 + v * 11) % 40;
                threshold = (threshold + val * 17) % 100;
                data->records[r * nvar + v] = (rng.next() % 100 < threshold) ? 1 : 0;
            }
        }
    }

    data->base_penalty = g_bayes_i > 0
        ? -0.5 * std::log((double)nrec) * g_bayes_i
        : 0.0;
    g_bayes = data;

    printf("Random seed                = %i\n", g_bayes_s);
    printf("Number of vars             = %i\n", g_bayes_v);
    printf("Number of records          = %i\n", g_bayes_r);
    printf("Max num parents            = %i\n", g_bayes_n);
    printf("%% chance of parent         = %i\n", g_bayes_p);
    printf("Insert penalty             = %i\n", g_bayes_i);
    printf("Max num edge learned / var = %i\n", g_bayes_e);
    printf("Operation quality factor   = %f\n", data->quality_factor);
    printf("Generating data... done.\n");
    printf("Learning structure...\n");
    fflush(stdout);
}

TX static int parents_has(BayesData* data, int var, int p) {
    int* pv = &data->parents[var * BAYES_MAX_PARENTS];
    int pc = data->parents_cnt[var];
    for (int i = 0; i < pc; i++)
        if (pv[i] == p) return 1;
    return 0;
}

TX static bool has_path(BayesData* data, int from, int to) {
    int nv = data->num_var;
    bool* visited = (bool*)__builtin_alloca(nv * sizeof(bool));
    for (int i = 0; i < nv; i++) visited[i] = false;
    int* stack = (int*)__builtin_alloca(nv * sizeof(int));
    int sp = 0;
    visited[from] = true;
    stack[sp++] = from;
    while (sp > 0) {
        int cur = stack[--sp];
        int* cv = &data->children_adj[cur * BAYES_MAX_PARENTS];
        int cc = data->children_cnt[cur];
        for (int i = 0; i < cc; i++) {
            int c = cv[i];
            if (c == to) return true;
            if (!visited[c]) { visited[c] = true; stack[sp++] = c; }
        }
    }
    return false;
}

TX static Task pop_best_task(BayesData* data) {
    if (task_heap_empty(data->task_count)) {
        Task t; t.op = -1; t.from_id = -1; t.to_id = -1; t.score = -1e100;
        return t;
    }
    Task t = task_heap_pop(data->task_list, data->task_count);
    return t;
}

TX static void insert_task(BayesData* data, const Task& t) {
    if (data->task_count < data->task_capacity)
        task_heap_push(data->task_list, data->task_count, t);
}

TX static bool apply_insert(BayesData* data, int from, int to) {
    if (parents_has(data, to, from)) return false;
    if (has_path(data, to, from)) return false;
    if (data->global_max_edges > 0 &&
        data->parents_cnt[to] >= data->global_max_edges) return false;

    int pc = data->parents_cnt[to];
    data->parents[to * BAYES_MAX_PARENTS + pc] = from;
    data->parents_cnt[to] = pc + 1;
    int cc = data->children_cnt[from];
    data->children_adj[from * BAYES_MAX_PARENTS + cc] = to;
    data->children_cnt[from] = cc + 1;
    data->total_parents++;

    int par[BAYES_MAX_PARENTS];
    for (int i = 0; i < pc; i++)
        par[i] = data->parents[to * BAYES_MAX_PARENTS + i];
    par[pc] = from;
    double new_ll = compute_density_ll(data, to, par, pc + 1);
    data->base_log_likelihood += (new_ll - data->local_ll[to]);
    data->local_ll[to] = new_ll;

    return true;
}

TX static Task find_best_insert(BayesData* data, int to) {
    Task best;
    best.op = -1; best.from_id = -1; best.to_id = -1; best.score = -1e100;
    double base_ll = data->local_ll[to];

    int pc = data->parents_cnt[to];
    for (int from = 0; from < data->num_var; from++) {
        if (from == to) continue;
        if (parents_has(data, to, from)) continue;
        if (has_path(data, to, from)) continue;
        if (data->global_max_edges > 0 && pc >= data->global_max_edges) continue;

        int par[BAYES_MAX_PARENTS];
        for (int i = 0; i < pc; i++)
            par[i] = data->parents[to * BAYES_MAX_PARENTS + i];
        par[pc] = from;
        double new_ll = compute_density_ll(data, to, par, pc + 1);
        double delta = new_ll - base_ll;
        double score = data->total_parents * data->base_penalty +
                       data->num_record * (data->base_log_likelihood + delta);

        if (score > best.score) {
            best.op = 0; best.from_id = from; best.to_id = to; best.score = score;
        }
    }
    return best;
}

static void init_worker(BayesData* data, int nvar, int start, int end) {
    for (int v = start; v < end; v++) {
        data->local_ll[v] = compute_density_ll(data, v, nullptr, 0);
        data->base_log_likelihood += data->local_ll[v];
    }
    for (int v = start; v < end; v++) {
        for (int from = 0; from < nvar; from++) {
            if (from == v) continue;
            int parent_arr[1] = {from};
            double with_ll = compute_density_ll(data, v, parent_arr, 1);
            if (with_ll > data->local_ll[v]) {
                double delta = with_ll - data->local_ll[v];
                double score = data->base_penalty +
                               data->num_record * (data->base_log_likelihood + delta);
                Task t;
                t.op = 0; t.from_id = from; t.to_id = v; t.score = score;
                if (data->task_count < data->task_capacity)
                    task_heap_push(data->task_list, data->task_count, t);
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
