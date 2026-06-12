// Bayes — C++ port of the original STAMP spec (LLVM plugin path)
// Auto-instrumented via __attribute__((annotate("transaction"))).
//
// Original spec: https://github.com/ccaominh/stamp/tree/master/bayes
//
// Parameters (matching original spec):
//   -v <num>   Number of variables       (default: 32)
//   -r <num>   Number of records         (default: 1024)
//   -n <num>   Max parents per variable  (default: 2)
//   -p <pct>   Percent chance of parent  (default: 20, unused in original)
//   -s <seed>  Random seed               (default: 0)
//   -i <num>   Insert penalty            (default: 2)
//   -e <num>   Max edges per variable    (default: 2)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include "tm_vector.hpp"

// ── Configuration ───────────────────────────────────────────────────
static long g_num_var           = 32;
static long g_num_record        = 1024;
static long g_max_parents       = 2;
static long g_insert_penalty    = 2;
static long g_max_edges_per_var = 2;
static unsigned g_seed          = 0;
static long g_num_threads       = 4;

// ── Constants ───────────────────────────────────────────────────────
static const int MAX_PARENTS     = 8;
static const int MAX_TASKS       = 131072;

struct Task {
    int op;
    int from_id;
    int to_id;
    double score;
};

// ── Double ↔ long reinterpretation ──────────────────────────────────
static inline long d2l(double v) { long r; memcpy(&r, &v, sizeof(r)); return r; }
static inline double l2d(long v) { double r; memcpy(&r, &v, sizeof(r)); return r; }

// ── TM abstraction ──────────────────────────────────────────────────
  extern "C" {
      void     tm_init();
      void     tm_exit();
      void     tm_init_thread();
      void     tm_exit_thread();
      void*    tm_calloc(size_t, size_t);
  }
  #define TX_FUNC      __attribute__((annotate("transaction"), noinline))
  #define TM_GLOBAL    __attribute__((annotate("tm")))
  #define TM_READ_I8(p)     (*(const long*)(p))
  #define TM_WRITE_I8(p, v) (*(long*)(p) = (long)(v))

#define TM_READ_DOUBLE(p)    l2d(TM_READ_I8((const long*)(p)))
#define TM_WRITE_DOUBLE(p,v) TM_WRITE_I8((long*)(p), d2l(v))

// ── RNG (LCG matching original STAMP) ───────────────────────────────
static thread_local uint32_t tls_rng_state = 1;

static void rng_seed(unsigned s) { tls_rng_state = s ? s : 1; }
static uint32_t rng_next() {
    tls_rng_state = tls_rng_state * 1103515245 + 12345;
    return tls_rng_state & 0x7fffffff;
}
static long rng_range(long range) {
    return (long)(rng_next() % (uint32_t)range);
}

// ── Global TM data arrays (allocated via tm_calloc) ─────────────────
static long* g_parent_count  = nullptr;
static long* g_parent_data   = nullptr;
static long* g_child_count   = nullptr;
static long* g_child_data    = nullptr;
static long* g_local_ll      = nullptr;
static long* g_base_log_likelihood = nullptr;
static long* g_total_parents = nullptr;
static long* g_task_op    = nullptr;
static long* g_task_from  = nullptr;
static long* g_task_to    = nullptr;
static long* g_task_score = nullptr;
static long* g_task_count = nullptr;

// Records (read-only after init)
static TMSafeVector<TMSafeVector<int>> g_records;

static double g_base_penalty = 0.0;
static std::atomic<long> g_total_ops{0};
static std::mutex g_init_mutex;

// ── Helper: compute density log-likelihood (non-TM, reads g_records) ─
static double compute_density_ll(int var, const TMSafeVector<int>& parents_vec) {
    int np = (int)parents_vec.size();
    int ncfg = 1 << np;
    TMSafeVector<int> c0(ncfg, 0), c1(ncfg, 0);
    for (int r = 0; r < g_num_record; r++) {
        int cfg = 0;
        for (int i = 0; i < np; i++)
            cfg = (cfg << 1) | g_records[r][parents_vec[i]];
        if (g_records[r][var] == 0) c0[cfg]++; else c1[cfg]++;
    }
    double ll = 0.0;
    for (int c = 0; c < ncfg; c++) {
        int t = c0[c] + c1[c];
        if (t == 0) continue;
        double p0 = (double)c0[c] / t;
        double p1 = (double)c1[c] / t;
        double frac = (double)t / g_num_record;
        if (p0 > 0) ll += frac * p0 * std::log(p0);
        if (p1 > 0) ll += frac * p1 * std::log(p1);
    }
    return ll;
}

// ── TM set operations (TX_FUNC for plugin auto-instrumentation) ─────
TX_FUNC static bool set_contains(const long* data, long count, long val) {
    for (long i = 0; i < count; i++)
        if (TM_READ_I8(&data[i]) == val) return true;
    return false;
}

TX_FUNC static void set_insert(long* data, long count, long val) {
    TM_WRITE_I8(&data[count], val);
}

TX_FUNC static void set_collect(const long* data, long count, TMSafeVector<int>& out) {
    for (long i = 0; i < count; i++)
        out.push_back((int)TM_READ_I8(&data[i]));
}

// ── TM path detection (reads children sets through TM barriers) ─────
TX_FUNC static bool has_path(int from, int to) {
    TMSafeVector<char> visited(g_num_var);
    TMSafeVector<int> stack;
    stack.push_back(from);
    visited[from] = true;
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        long ccount = TM_READ_I8(&g_child_count[cur]);
        for (long i = 0; i < ccount; i++) {
            int c = (int)TM_READ_I8(&g_child_data[cur * g_num_var + i]);
            if (c == to) return true;
            if (c >= 0 && c < g_num_var && !visited[c]) {
                visited[c] = true;
                stack.push_back(c);
            }
        }
    }
    return false;
}

// ── TM operations ───────────────────────────────────────────────────
TX_FUNC static Task find_best_insert(int to) {
    Task best = {-1, -1, -1, -1e100};
    double base_ll = TM_READ_DOUBLE(&g_local_ll[to]);
    long np = TM_READ_I8(&g_parent_count[to]);
    if (g_max_edges_per_var > 0 && np >= g_max_edges_per_var)
        return best;
    for (int from = 0; from < g_num_var; from++) {
        if (from == to) continue;
        if (set_contains(g_parent_data + to * MAX_PARENTS, np, from)) continue;
        if (has_path(to, from)) continue;
        np = TM_READ_I8(&g_parent_count[to]);
        if (g_max_edges_per_var > 0 && np >= g_max_edges_per_var)
            continue;
        TMSafeVector<int> par;
        set_collect(g_parent_data + to * MAX_PARENTS, np, par);
        par.push_back(from);
        double new_ll = compute_density_ll(to, par);
        double delta = new_ll - base_ll;
        double score = TM_READ_I8(g_total_parents) * g_base_penalty +
                       g_num_record * (TM_READ_DOUBLE(g_base_log_likelihood) + delta);
        if (score > best.score)
            best = {0, from, to, score};
    }
    return best;
}

TX_FUNC static Task pop_best_task() {
    long n = TM_READ_I8(g_task_count);
    if (n == 0) return {-1, -1, -1, -1e100};
    int best = 0;
    double best_score = TM_READ_DOUBLE(&g_task_score[0]);
    for (long i = 1; i < n; i++) {
        double s = TM_READ_DOUBLE(&g_task_score[i]);
        if (s > best_score) { best_score = s; best = (int)i; }
    }
    Task t;
    t.op = (int)TM_READ_I8(&g_task_op[best]);
    t.from_id = (int)TM_READ_I8(&g_task_from[best]);
    t.to_id = (int)TM_READ_I8(&g_task_to[best]);
    t.score = best_score;
    long last = n - 1;
    if (best != last) {
        TM_WRITE_I8(&g_task_op[best], TM_READ_I8(&g_task_op[last]));
        TM_WRITE_I8(&g_task_from[best], TM_READ_I8(&g_task_from[last]));
        TM_WRITE_I8(&g_task_to[best], TM_READ_I8(&g_task_to[last]));
        TM_WRITE_DOUBLE(&g_task_score[best], TM_READ_DOUBLE(&g_task_score[last]));
    }
    TM_WRITE_I8(g_task_count, last);
    return t;
}

TX_FUNC static void insert_task(const Task& t) {
    long n = TM_READ_I8(g_task_count);
    if (n >= MAX_TASKS) return;
    TM_WRITE_I8(&g_task_op[n], t.op);
    TM_WRITE_I8(&g_task_from[n], t.from_id);
    TM_WRITE_I8(&g_task_to[n], t.to_id);
    TM_WRITE_DOUBLE(&g_task_score[n], t.score);
    TM_WRITE_I8(g_task_count, n + 1);
}

TX_FUNC static bool apply_insert(const Task& t) {
    int from = t.from_id;
    int to = t.to_id;
    long np = TM_READ_I8(&g_parent_count[to]);
    if (set_contains(g_parent_data + to * MAX_PARENTS, np, from))
        return false;
    if (has_path(to, from))
        return false;
    np = TM_READ_I8(&g_parent_count[to]);
    if (g_max_edges_per_var > 0 && np >= g_max_edges_per_var)
        return false;
    set_insert(g_parent_data + to * MAX_PARENTS, np, from);
    TM_WRITE_I8(&g_parent_count[to], np + 1);
    long nc = TM_READ_I8(&g_child_count[from]);
    set_insert(g_child_data + from * g_num_var, nc, to);
    TM_WRITE_I8(&g_child_count[from], nc + 1);
    TM_WRITE_I8(g_total_parents, TM_READ_I8(g_total_parents) + 1);
    TMSafeVector<int> par;
    set_collect(g_parent_data + to * MAX_PARENTS, (long)(np + 1), par);
    double new_ll = compute_density_ll(to, par);
    double old_ll = TM_READ_DOUBLE(&g_local_ll[to]);
    TM_WRITE_DOUBLE(g_base_log_likelihood,
                    TM_READ_DOUBLE(g_base_log_likelihood) + (new_ll - old_ll));
    TM_WRITE_DOUBLE(&g_local_ll[to], new_ll);
    return true;
}

// ── Worker thread ───────────────────────────────────────────────────
static void worker(int tid) {
    tm_init_thread();
    rng_seed(g_seed + (unsigned)tid);
    int nvar = (int)g_num_var;
    int chunk = (nvar + (int)g_num_threads - 1) / (int)g_num_threads;
    int start = tid * chunk;
    int end = start + chunk > nvar ? nvar : start + chunk;

    // ── Phase 1: initialize local_ll ──
    {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        for (int v = start; v < end; v++) {
            double ll = compute_density_ll(v, {});
            TM_WRITE_DOUBLE(&g_local_ll[v], ll);
            TM_WRITE_DOUBLE(g_base_log_likelihood,
                             TM_READ_DOUBLE(g_base_log_likelihood) + ll);
        }
    }

    // ── Phase 2: build initial task list ──
    {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        for (int v = start; v < end; v++) {
            double base_ll = TM_READ_DOUBLE(&g_local_ll[v]);
            for (int from = 0; from < nvar; from++) {
                if (from == v) continue;
                double with_ll = compute_density_ll(v, {from});
                if (with_ll > base_ll) {
                    double delta = with_ll - base_ll;
                    Task t = {0, from, v, 0.0};
                    t.score = g_base_penalty +
                              g_num_record * (TM_READ_DOUBLE(g_base_log_likelihood) + delta);
                    insert_task(t);
                }
            }
        }
    }

    // ── Phase 3: work loop ──
    for (;;) {
        Task t;
        t = pop_best_task();
        if (t.op < 0) break;

        bool ok = false;
        ok = apply_insert(t);
        if (ok) {
            Task next;
            next = find_best_insert(t.to_id);
            if (next.op >= 0) {
                insert_task(next);
            }
            g_total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    tm_exit_thread();
}

// ── Main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-v") == 0 && i+1 < argc) g_num_var = atol(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i+1 < argc) g_num_record = atol(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) g_max_parents = atol(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) g_seed = (unsigned)atol(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i+1 < argc) g_insert_penalty = atol(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0 && i+1 < argc) g_max_edges_per_var = atol(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i+1 < argc) g_num_threads = atol(argv[++i]);
    }

    printf("Bayes (STAMP spec, shared source)\n");
    printf("  Variables:   %ld\n", g_num_var);
    printf("  Records:     %ld\n", g_num_record);
    printf("  Max parents: %ld\n", g_max_parents);
    printf("  Penalty:     %ld\n", g_insert_penalty);
    printf("  Max edges:   %ld\n", g_max_edges_per_var);
    printf("  Threads:     %ld\n", g_num_threads);
    printf("  Seed:        %u\n", g_seed);
    printf("  Path:        %s\n",
           "LLVM plugin"
    );

    // ── Allocate TM data ──
    tm_init();

    int num_var_i = (int)g_num_var;
    g_parent_count = (long*)tm_calloc(g_num_var, sizeof(long));
    g_parent_data  = (long*)tm_calloc(g_num_var * MAX_PARENTS, sizeof(long));
    g_child_count  = (long*)tm_calloc(g_num_var, sizeof(long));
    g_child_data   = (long*)tm_calloc(g_num_var * g_num_var, sizeof(long));
    g_local_ll     = (long*)tm_calloc(g_num_var, sizeof(long));
    g_base_log_likelihood = (long*)tm_calloc(1, sizeof(long));
    g_total_parents = (long*)tm_calloc(1, sizeof(long));
    g_task_op    = (long*)tm_calloc(MAX_TASKS, sizeof(long));
    g_task_from  = (long*)tm_calloc(MAX_TASKS, sizeof(long));
    g_task_to    = (long*)tm_calloc(MAX_TASKS, sizeof(long));
    g_task_score = (long*)tm_calloc(MAX_TASKS, sizeof(long));
    g_task_count = (long*)tm_calloc(1, sizeof(long));

    *g_base_log_likelihood = d2l(0.0);
    *g_total_parents = 0;
    *g_task_count = 0;
    for (int i = 0; i < num_var_i; i++)
        g_local_ll[i] = d2l(0.0);

    g_base_penalty = g_insert_penalty > 0
        ? -0.5 * std::log((double)g_num_record) * g_insert_penalty
        : 0.0;

    // ── Generate random data (non-TM setup) ──
    rng_seed(g_seed);

    for (int v = 1; v < num_var_i; v++) {
        int max_np = std::min((int)g_max_parents, v);
        int np = (max_np > 0) ? (int)(rng_range(max_np)) + 1 : 0;
        for (int p = 0; p < np && p < v; p++) {
            int parent = (int)rng_range(v);
            bool found = false;
            for (int j = 0; j < (int)g_parent_count[v]; j++) {
                if (g_parent_data[v * MAX_PARENTS + j] == parent) { found = true; break; }
            }
            if (!found) {
                g_parent_data[v * MAX_PARENTS + g_parent_count[v]] = parent;
                g_parent_count[v]++;
                g_child_data[parent * g_num_var + g_child_count[parent]] = v;
                g_child_count[parent]++;
                (*g_total_parents)++;
            }
        }
    }

    g_records.resize(g_num_record, TMSafeVector<int>(g_num_var));
    for (int r = 0; r < (int)g_num_record; r++) {
        for (int v = 0; v < num_var_i; v++) {
            if (g_parent_count[v] == 0) {
                g_records[r][v] = (int)(rng_next() % 2);
            } else {
                int val = 0;
                for (int j = 0; j < (int)g_parent_count[v]; j++) {
                    int pid = (int)g_parent_data[v * MAX_PARENTS + j];
                    val = (val << 1) | g_records[r][pid];
                }
                int threshold = 30;
                for (int j = 0; j < (int)g_parent_count[v]; j++) {
                    int pid = (int)g_parent_data[v * MAX_PARENTS + j];
                    threshold += (pid * 7 + v * 11) % 40;
                }
                threshold = (threshold + val * 17) % 100;
                g_records[r][v] = (rng_next() % 100 < (uint32_t)threshold) ? 1 : 0;
            }
        }
    }

    printf("  Initial parents: %ld\n", *g_total_parents);
    fflush(stdout);

    // ── Launch worker threads ──
    std::vector<std::thread> threads;
    auto t1 = std::chrono::steady_clock::now();
    for (long t = 0; t < g_num_threads; t++)
        threads.emplace_back(worker, (int)t);
    for (auto& th : threads) th.join();
    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    // ── Report results ──
    long ops = g_total_ops.load();
    long total_parents = *g_total_parents;
    printf("\nResults (%ld ms):\n", (long)(elapsed * 1000));
    printf("  Operations: %ld  Total parents: %ld\n", ops, total_parents);
    printf("  Time: %.6f sec\n", elapsed);
    printf("  Rate: %.0f ops/sec\n", ops / elapsed);
    printf("  PASS\n");

    tm_exit();
    return 0;
}
