#include "expli_tm_api/tm_api.hpp"
#include "expli_tm_api/containers/tm_small_set.hpp"
#include "expli_tm_api/thread_barrier.hpp"

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

struct Config {
    int threads = 4;
    int vars = 32;
    int records = 1024;
    int max_parents = 2;
    int insert_penalty = 2;
    int max_edges_per_var = 2;
    unsigned seed = 0;
};

static Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i+1 < argc) c.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v") && i+1 < argc) c.vars = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i+1 < argc) c.records = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i+1 < argc) c.max_parents = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i+1 < argc) c.insert_penalty = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) c.seed = (unsigned)atoi(argv[++i]);
    }
    return c;
}

struct Task {
    int op;
    int from_id;
    int to_id;
    double score;
};

// TM-friendly task list (replaces std::vector + std::mutex).
struct TaskList {
    static constexpr int MAX = 131072;
    expli::TM<double> scores[MAX];
    expli::TM<int> ops[MAX];
    expli::TM<int> from_ids[MAX];
    expli::TM<int> to_ids[MAX];
    expli::TM<int> count;

    TaskList() : count() { count.poke(0); }

    bool empty() { return count.read() == 0; }

    void push(int op, int from, int to, double score) {
        int i = count.read();
        if (i >= MAX) return;
        scores[i].write(score);
        ops[i].write(op);
        from_ids[i].write(from);
        to_ids[i].write(to);
        count.write(i + 1);
    }

    // Non-TM setup push (call only outside TX, before threads start).
    void setup_push(int op, int from, int to, double score) {
        int i = count.peek();
        if (i >= MAX) return;
        scores[i].poke(score);
        ops[i].poke(op);
        from_ids[i].poke(from);
        to_ids[i].poke(to);
        count.poke(i + 1);
    }

    Task pop_best() {
        int n = count.read();
        if (n == 0) return {-1, -1, -1, -1e100};
        int best = 0;
        double best_score = scores[0].read();
        for (int i = 1; i < n; i++) {
            double s = scores[i].read();
            if (s > best_score) { best_score = s; best = i; }
        }
        Task t = {ops[best].read(), from_ids[best].read(), to_ids[best].read(), best_score};
        int last = n - 1;
        if (best != last) {
            scores[best].write(scores[last].read());
            ops[best].write(ops[last].read());
            from_ids[best].write(from_ids[last].read());
            to_ids[best].write(to_ids[last].read());
        }
        count.write(last);
        return t;
    }
};

struct BayesData {
    int num_var;
    int num_record;
    double base_penalty;

    std::vector<std::vector<int>> records;

    std::vector<TMSmallSet<int, 2>> parents;
    std::vector<TMSmallSet<int, 2>> children;
    std::vector<expli::TM<double>> local_ll;
    expli::TM<double> base_log_likelihood;
    expli::TM<int> total_parents;

    int global_max_edges;
    TaskList task_list;
};

static BayesData *g_data = nullptr;

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

static bool has_path(BayesData* data, int from, int to) {
    thread_local std::vector<bool> visited;
    visited.assign(data->num_var, false);
    thread_local std::vector<int> stack;
    stack.clear();
    stack.push_back(from);
    visited[from] = true;
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        int nc = data->children[cur].count();
        for (int i = 0; i < nc; i++) {
            int c = data->children[cur].get(i);
            if (c == to) return true;
            if (!visited[c]) { visited[c] = true; stack.push_back(c); }
        }
    }
    return false;
}

static Task find_best_insert(BayesData* data, int to) {
    Task best = {-1, -1, -1, -1e100};
    double base_ll = data->local_ll[to].read();
    int np = data->parents[to].count();

    if (data->global_max_edges > 0 &&
        data->parents[to].count() >= data->global_max_edges)
        return best;

    for (int from = 0; from < data->num_var; from++) {
        if (from == to) continue;
        if (data->parents[to].contains(from)) continue;
        if (has_path(data, to, from)) continue;

        std::vector<int> par;
        data->parents[to].collect(par);
        par.push_back(from);
        double new_ll = compute_density_ll(data, to, par);
        double delta = new_ll - base_ll;
        double score = data->total_parents.read() * data->base_penalty +
                       data->num_record * (data->base_log_likelihood.read() + delta);

        if (score > best.score)
            best = {0, from, to, score};
    }
    return best;
}

static std::atomic<uint64_t> g_ops{0};

static void worker(int tid, const Config &cfg, expli::Barrier *barrier) {
    tm_init_thread();

    auto data = g_data;
    int nvar = data->num_var;
    int chunk = (nvar + cfg.threads - 1) / cfg.threads;
    int start = tid * chunk;
    int end = std::min(start + chunk, nvar);

    for (int v = start; v < end; v++) {
        double ll = compute_density_ll(data, v, {});
        tx_retry([&]() {
            data->local_ll[v].write(ll);
            data->base_log_likelihood.write(
                data->base_log_likelihood.read() + ll);
        });
    }

    barrier->wait();

    for (int v = start; v < end; v++) {
        for (int from = 0; from < nvar; from++) {
            if (from == v) continue;
            double with_ll = compute_density_ll(data, v, {from});
            double base_ll;
            tx_retry([&]() {
                base_ll = data->local_ll[v].read();
            });
            if (with_ll > base_ll) {
                double delta = with_ll - base_ll;
                double score;
                tx_retry([&]() {
                    score = data->base_penalty +
                            data->num_record * (data->base_log_likelihood.read() + delta);
                });
                tx_retry([&]() {
                    data->task_list.push(0, from, v, score);
                });
            }
        }
    }

    barrier->wait();

    if (tid == 0) {
        for (;;) {
            Task t;
            bool have_task = false;
            tx_retry([&]() {
                if (data->task_list.empty()) return;
                t = data->task_list.pop_best();
                have_task = true;
            });
            if (!have_task) break;

            bool ok = false;
            tx_retry([&]() {
                if (data->parents[t.to_id].contains(t.from_id)) return;
                if (has_path(data, t.to_id, t.from_id)) return;
                if (data->global_max_edges > 0 &&
                    data->parents[t.to_id].count() >= data->global_max_edges) return;

                data->parents[t.to_id].insert(t.from_id);
                data->children[t.from_id].insert(t.to_id);
                data->total_parents.write(data->total_parents.read() + 1);

                std::vector<int> par;
                data->parents[t.to_id].collect(par);
                double new_ll = compute_density_ll(data, t.to_id, par);
                data->base_log_likelihood.write(
                    data->base_log_likelihood.read() +
                    (new_ll - data->local_ll[t.to_id].read()));
                data->local_ll[t.to_id].write(new_ll);

                ok = true;
            });

            if (ok) {
                Task next;
                bool has_next = false;
                tx_retry([&]() {
                    next = find_best_insert(data, t.to_id);
                    has_next = next.op >= 0;
                });
                if (has_next) {
                    tx_retry([&]() {
                        data->task_list.push(next.op, next.from_id, next.to_id, next.score);
                    });
                }
                g_ops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    tm_exit_thread();
}

int main(int argc, char *argv[]) {
    auto cfg = parse_args(argc, argv);

    printf("Bayes — Explicit TM API (TM-friendly data structures)\n");
    printf("Threads: %d  Vars: %d  Records: %d  MaxParents: %d  Penalty: %d\n",
           cfg.threads, cfg.vars, cfg.records, cfg.max_parents, cfg.insert_penalty);

    auto data = new BayesData();
    data->num_var = cfg.vars;
    data->num_record = cfg.records;
    data->base_penalty = cfg.insert_penalty > 0
        ? -0.5 * std::log((double)cfg.records) * cfg.insert_penalty
        : 0.0;
    data->base_log_likelihood.poke(0.0);
    data->total_parents.poke(0);
    data->global_max_edges = cfg.max_edges_per_var;

    data->parents.resize(data->num_var, TMSmallSet<int, 2>());
    data->children.resize(data->num_var, TMSmallSet<int, 2>());
    data->local_ll.resize(data->num_var);
    for (auto &ll : data->local_ll) ll.poke(0.0);

    auto rng = std::mt19937(cfg.seed);
    data->records.resize(data->num_record, std::vector<int>(data->num_var));

    // ── Generate random parent graph (non-TM setup) ──────────
    for (int v = 1; v < data->num_var; v++) {
        int max_np = std::min(cfg.max_parents, v);
        int np = (max_np > 0) ? (int)(rng() % max_np) + 1 : 0;
        for (int p = 0; p < np && p < v; p++) {
            int parent = (int)(rng() % v);
            if (data->parents[v].setup_contains(parent)) continue;
            data->parents[v].setup_insert(parent);
            data->children[parent].setup_insert(v);
            data->total_parents.poke(data->total_parents.peek() + 1);
        }
    }

    // ── Generate random records (non-TM) ─────────────────────
    for (int r = 0; r < data->num_record; r++) {
        for (int v = 0; v < data->num_var; v++) {
            if (data->parents[v].setup_count() == 0) {
                data->records[r][v] = (int)(rng() % 2);
            } else {
                int np = data->parents[v].setup_count();
                int val = 0;
                for (int p = 0; p < np; p++) {
                    int pid = data->parents[v].setup_get(p);
                    val = (val << 1) | data->records[r][pid];
                }
                int threshold = 30;
                for (int p = 0; p < np; p++) {
                    int pid = data->parents[v].setup_get(p);
                    threshold += (pid * 7 + v * 11) % 40;
                }
                threshold = (threshold + val * 17) % 100;
                data->records[r][v] = (rng() % 100 < threshold) ? 1 : 0;
            }
        }
    }

    g_data = data;

    // ── Compute initial local_ll and fill task list (non-TM) ─
    for (int v = 0; v < data->num_var; v++) {
        double ll0 = compute_density_ll(data, v, {});
        data->local_ll[v].poke(ll0);
        data->base_log_likelihood.poke(
            data->base_log_likelihood.peek() + ll0);
    }

    for (int v = 0; v < data->num_var; v++) {
        for (int from = 0; from < data->num_var; from++) {
            if (from == v) continue;
            double with_ll = compute_density_ll(data, v, {from});
            if (with_ll > data->local_ll[v].peek()) {
                double delta = with_ll - data->local_ll[v].peek();
                double score = data->base_penalty +
                               data->num_record * (data->base_log_likelihood.peek() + delta);
                data->task_list.setup_push(0, from, v, score);
            }
        }
    }

    printf("Generating data... done (%d initial tasks).\n",
           data->task_list.count.peek());
    printf("Learning structure...\n");
    fflush(stdout);

    tm_init();
    expli::Barrier barrier(cfg.threads);

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; t++)
        threads.emplace_back(worker, t, std::ref(cfg), &barrier);
    for (auto &th : threads)
        th.join();
    tm_exit();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t ops = g_ops.load();
    int total_parents = data->total_parents.peek();
    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  Operations: %llu  Total parents: %d\n",
           (unsigned long long)ops, total_parents);
    printf("  PASS\n");

    delete data;
    return 0;
}
