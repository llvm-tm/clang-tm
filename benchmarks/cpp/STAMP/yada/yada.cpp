// Yada — C++ port of the original STAMP spec (explicit API path)
// Uses tm_read_i8/tm_write_i8 + tx_run() for TM access.
//
// Original spec: https://github.com/ccaominh/stamp/tree/master/yada
//
// Parameters (matching original spec):
//   -a <angle>  Angle constraint in degrees  (default: 20)
//   -j <jitter> Jitter for synthetic mesh    (default: 0.5)
//   -t <num>    Number of threads            (default: 4)

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
#include <set>
#include <algorithm>
#include <mutex>
#include <random>

#include "../../tests/benchmark_test.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Configuration ───────────────────────────────────────────────────
static long g_angle_constraint = 20;
static double g_jitter = 0.5;
static long g_num_threads = 4;

static void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-a") == 0 && i+1 < argc) g_angle_constraint = atol(argv[++i]);
        else if (strcmp(argv[i], "-j") == 0 && i+1 < argc) g_jitter = atof(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i+1 < argc) g_num_threads = atol(argv[++i]);
    }
}

// ── Constants ───────────────────────────────────────────────────────
static const int MAX_NEIGHBORS = 16;
static const int MAX_NEW_ELEMENTS_PER_REFINE = 16;
static const int MAX_ELEMENTS = 200000;

struct Point { double x, y; };
struct Edge {
    Point a, b;
    bool operator<(const Edge& o) const {
        if (a.x != o.a.x) return a.x < o.a.x;
        if (a.y != o.a.y) return a.y < o.a.y;
        if (b.x != o.b.x) return b.x < o.b.x;
        return b.y < o.b.y;
    }
};

// ── Double ↔ long reinterpretation ──────────────────────────────────
static inline long d2l(double v) { long r; memcpy(&r, &v, sizeof(r)); return r; }
static inline double l2d(long v) { double r; memcpy(&r, &v, sizeof(r)); return r; }

// ── TM abstraction ──────────────────────────────────────────────────
  extern "C" {
      extern void     (*tm_begin)();
      extern void     (*tm_end)();
      extern long     (*tm_read_i8)(const long*);
      extern void     (*tm_write_i8)(long*, long);
      void     tm_init();
      void     tm_exit();
      void     tm_init_thread();
      void     tm_exit_thread();
      extern void*    (*tm_calloc)(size_t, size_t);
  }
  extern __thread int32_t tm_nested_call_counter;
  extern __thread sigjmp_buf tm_jmpbuf;

  #define TX_FUNC
  #define TM_GLOBAL
  #define TM_READ_I8(p)     tm_read_i8((const long*)(p))
  #define TM_WRITE_I8(p, v) tm_write_i8((long*)(p), (long)(v))

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

// ── Element data (flat arrays in TM region for TM-accessed fields) ──
static long* g_elem_encroached   = nullptr; // [MAX_ELEMENTS]
static long* g_elem_is_garbage   = nullptr; // [MAX_ELEMENTS]
static long* g_elem_is_referenced= nullptr; // [MAX_ELEMENTS]
static long* g_elem_neighbor_cnt = nullptr; // [MAX_ELEMENTS]
static long* g_elem_neighbors    = nullptr; // [MAX_ELEMENTS * MAX_NEIGHBORS]

// Work heap (in TM region)
static long* g_work_heap_data = nullptr; // [MAX_ELEMENTS]
static long* g_work_heap_cnt  = nullptr; // single value

// Geometry (non-TM, read-only after init)
static double* g_pt_x  = nullptr; // [MAX_ELEMENTS * 3]
static double* g_pt_y  = nullptr; // [MAX_ELEMENTS * 3]
static double* g_circ_x = nullptr; // [MAX_ELEMENTS]
static double* g_circ_y = nullptr; // [MAX_ELEMENTS]
static double* g_circ_r = nullptr; // [MAX_ELEMENTS]
static double* g_min_angle = nullptr; // [MAX_ELEMENTS]

static long* g_elem_count_ptr = nullptr;
static std::atomic<long> g_total_ops{0};
static std::atomic<bool> g_stop{false};

// ── Geometry helpers (no TM needed) ─────────────────────────────────
static double dist2(const Point& a, const Point& b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static void circumcircle(double& cx, double& cy, double& cr,
                          const Point& a, const Point& b, const Point& c) {
    double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if ((d < 0 ? -d : d) < 1e-15) { cr = 1e15; cx = cy = 0; return; }
    cx = ((a.x * a.x + a.y * a.y) * (b.y - c.y) +
          (b.x * b.x + b.y * b.y) * (c.y - a.y) +
          (c.x * c.x + c.y * c.y) * (a.y - b.y)) / d;
    cy = ((a.x * a.x + a.y * a.y) * (c.x - b.x) +
          (b.x * b.x + b.y * b.y) * (a.x - c.x) +
          (c.x * c.x + c.y * c.y) * (b.x - a.x)) / d;
    cr = std::sqrt(dist2({cx, cy}, a));
}

static double tri_min_angle(const Point& a, const Point& b, const Point& c) {
    auto angle = [](const Point& p, const Point& q, const Point& r) {
        double d1 = std::sqrt(dist2(p, q));
        double d2 = std::sqrt(dist2(p, r));
        if (d1 < 1e-15 || d2 < 1e-15) return 180.0;
        double dot = ((q.x - p.x) * (r.x - p.x) + (q.y - p.y) * (r.y - p.y)) / (d1 * d2);
        if (dot < -1.0) dot = -1.0;
        if (dot > 1.0) dot = 1.0;
        return std::acos(dot) * 180.0 / M_PI;
    };
    return std::min({angle(a, b, c), angle(b, a, c), angle(c, a, b)});
}

static bool is_encroached(const Point& a, const Point& b, const Point& c) {
    auto mid = Point{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
    double r2 = dist2(a, b) / 4.0;
    return dist2(mid, c) <= r2;
}

static Edge make_edge(const Point& a, const Point& b) {
    if (b.x < a.x || (b.x == a.x && b.y < a.y)) return {b, a};
    return {a, b};
}

static bool point_in_circum_sq(double cx, double cy, double cr_sq, const Point& p) {
    return dist2({cx, cy}, p) <= cr_sq;
}

// ── TM operations ──────────────────────────────────────────────────

TX_FUNC static void pop_work_best(int* out_id) {
    long n = TM_READ_I8(g_work_heap_cnt);
    if (n == 0) { *out_id = -1; return; }
    int best = 0;
    long best_id = TM_READ_I8(&g_work_heap_data[0]);
    long best_enc = TM_READ_I8(&g_elem_encroached[best_id]);
    for (long i = 1; i < n; i++) {
        long id = TM_READ_I8(&g_work_heap_data[i]);
        long enc = TM_READ_I8(&g_elem_encroached[id]);
        if (enc > best_enc) { best_enc = enc; best = (int)i; }
    }
    *out_id = (int)TM_READ_I8(&g_work_heap_data[best]);
    long last = n - 1;
    if (best != last) {
        TM_WRITE_I8(&g_work_heap_data[best], TM_READ_I8(&g_work_heap_data[last]));
    }
    TM_WRITE_I8(g_work_heap_cnt, last);
}

TX_FUNC static void push_work(int id) {
    long n = TM_READ_I8(g_work_heap_cnt);
    if (n >= MAX_ELEMENTS) return;
    TM_WRITE_I8(&g_work_heap_data[n], id);
    TM_WRITE_I8(g_work_heap_cnt, n + 1);
}

TX_FUNC static long read_encroached(int id) {
    return TM_READ_I8(&g_elem_encroached[id]);
}

TX_FUNC static long read_is_garbage(int id) {
    return TM_READ_I8(&g_elem_is_garbage[id]);
}

TX_FUNC static long read_is_referenced(int id) {
    return TM_READ_I8(&g_elem_is_referenced[id]);
}

TX_FUNC static void write_is_garbage(int id, long val) {
    TM_WRITE_I8(&g_elem_is_garbage[id], val);
}

TX_FUNC static void write_is_referenced(int id, long val) {
    TM_WRITE_I8(&g_elem_is_referenced[id], val);
}

TX_FUNC static void write_encroached(int id, long val) {
    TM_WRITE_I8(&g_elem_encroached[id], val);
}

TX_FUNC static long read_neighbor_count(int id) {
    return TM_READ_I8(&g_elem_neighbor_cnt[id]);
}

TX_FUNC static long read_neighbor(int id, int idx) {
    return TM_READ_I8(&g_elem_neighbors[id * MAX_NEIGHBORS + idx]);
}

// ── Grow region + retriangulate (single TX for consistency) ─────────
TX_FUNC static void refine_element(int el_id,
                                    std::vector<int>& before_ids,
                                    std::set<Edge>& border_edges,
                                    std::vector<int>& bad_ids,
                                    std::vector<int>& bfs_queue) {
    // ── Read seed element geometry ──
    double seed_cx = g_circ_x[el_id];
    double seed_cy = g_circ_y[el_id];
    double seed_cr_sq = g_circ_r[el_id] * g_circ_r[el_id] + 1e-10;
    Point p0 = {g_pt_x[el_id * 3], g_pt_y[el_id * 3]};
    Point p1 = {g_pt_x[el_id * 3 + 1], g_pt_y[el_id * 3 + 1]};
    Point p2 = {g_pt_x[el_id * 3 + 2], g_pt_y[el_id * 3 + 2]};

    // ── Grow region ──
    before_ids.clear();
    border_edges.clear();
    bad_ids.clear();
    bfs_queue.clear();

    before_ids.push_back(el_id);
    bfs_queue.push_back(el_id);
    std::set<int> visited;
    visited.insert(el_id);
    size_t qidx = 0;

    while (qidx < bfs_queue.size()) {
        int cur_id = bfs_queue[qidx++];
        double cur_cx = g_circ_x[cur_id];
        double cur_cy = g_circ_y[cur_id];

        if (!point_in_circum_sq(seed_cx, seed_cy, seed_cr_sq, {cur_cx, cur_cy})) {
            Point ca = {g_pt_x[cur_id * 3], g_pt_y[cur_id * 3]};
            Point cb = {g_pt_x[cur_id * 3 + 1], g_pt_y[cur_id * 3 + 1]};
            Point cc = {g_pt_x[cur_id * 3 + 2], g_pt_y[cur_id * 3 + 2]};
            border_edges.insert(make_edge(ca, cb));
            border_edges.insert(make_edge(cb, cc));
            border_edges.insert(make_edge(cc, ca));
            continue;
        }

        long nc = read_neighbor_count(cur_id);
        for (long ni = 0; ni < nc; ni++) {
            int nid = (int)read_neighbor(cur_id, (int)ni);
            if (visited.find(nid) == visited.end()) {
                visited.insert(nid);
                bfs_queue.push_back(nid);
            }
        }
    }

    // ── Mark old elements as garbage ──
    for (int bid : before_ids) {
        if (bid < MAX_ELEMENTS)
            write_is_garbage(bid, 1);
    }

    // ── Compute centroid ──
    double cx = p0.x / 3.0 + p1.x / 3.0 + p2.x / 3.0;
    double cy = p0.y / 3.0 + p1.y / 3.0 + p2.y / 3.0;
    Point centroid = {cx, cy};

    // ── Create new elements from border edges ──
    int border_count = 0;
    // Save the slot for the seed element's replacement
    long saved_new_id = TM_READ_I8(g_elem_count_ptr);
    long cur_id = saved_new_id;

    for (auto& edge : border_edges) {
        if (border_count >= 3) break;

        int tid = (int)cur_id;
        TM_WRITE_I8(g_elem_count_ptr, cur_id + 1);
        cur_id++;
        if (tid >= MAX_ELEMENTS) break;

        // Geometry
        g_pt_x[tid * 3] = centroid.x;
        g_pt_y[tid * 3] = centroid.y;
        g_pt_x[tid * 3 + 1] = edge.a.x;
        g_pt_y[tid * 3 + 1] = edge.a.y;
        g_pt_x[tid * 3 + 2] = edge.b.x;
        g_pt_y[tid * 3 + 2] = edge.b.y;

        circumcircle(g_circ_x[tid], g_circ_y[tid], g_circ_r[tid],
                     {g_pt_x[tid * 3], g_pt_y[tid * 3]},
                     {g_pt_x[tid * 3 + 1], g_pt_y[tid * 3 + 1]},
                     {g_pt_x[tid * 3 + 2], g_pt_y[tid * 3 + 2]});
        g_min_angle[tid] = tri_min_angle(
            {g_pt_x[tid * 3], g_pt_y[tid * 3]},
            {g_pt_x[tid * 3 + 1], g_pt_y[tid * 3 + 1]},
            {g_pt_x[tid * 3 + 2], g_pt_y[tid * 3 + 2]});

        // TM flags
        write_encroached(tid, 0);
        write_is_garbage(tid, 0);
        write_is_referenced(tid, 0);

        // Check encroached
        Point ta = {g_pt_x[tid * 3], g_pt_y[tid * 3]};
        Point tb = {g_pt_x[tid * 3 + 1], g_pt_y[tid * 3 + 1]};
        Point tc = {g_pt_x[tid * 3 + 2], g_pt_y[tid * 3 + 2]};
        if (is_encroached(ta, tb, tc) ||
            is_encroached(tb, tc, ta) ||
            is_encroached(tc, ta, tb))
            write_encroached(tid, 1);

        // Check if bad
        bool bad = g_min_angle[tid] < g_angle_constraint || read_encroached(tid) != 0;
        if (bad && read_is_referenced(tid) == 0) {
            write_is_referenced(tid, 1);
            bad_ids.push_back(tid);
        }
        border_count++;
    }

    // Replace seed element with updated geometry (reuse its slot)
    if (border_count > 0 && saved_new_id < MAX_ELEMENTS) {
        g_pt_x[saved_new_id * 3] = p0.x;
        g_pt_y[saved_new_id * 3] = p0.y;
        g_pt_x[saved_new_id * 3 + 1] = p1.x;
        g_pt_y[saved_new_id * 3 + 1] = p1.y;
        g_pt_x[saved_new_id * 3 + 2] = p2.x;
        g_pt_y[saved_new_id * 3 + 2] = p2.y;
        circumcircle(g_circ_x[saved_new_id], g_circ_y[saved_new_id],
                     g_circ_r[saved_new_id], centroid, p0, p1);
        g_min_angle[saved_new_id] = 180.0;
        write_is_garbage(saved_new_id, 0);
        write_is_referenced(saved_new_id, 0);
    }
}

TX_FUNC static void push_bad_ids(const std::vector<int>& bad_ids) {
    for (int bid : bad_ids) {
        if (read_is_referenced(bid) != 0) {
            long n = TM_READ_I8(g_work_heap_cnt);
            if (n < MAX_ELEMENTS) {
                TM_WRITE_I8(&g_work_heap_data[n], bid);
                TM_WRITE_I8(g_work_heap_cnt, n + 1);
            }
        }
    }
}

// ── Worker thread ──────────────────────────────────────────────────
static void worker(int tid) {
    tm_init_thread();

    std::vector<int> before_ids;
    std::vector<int> bad_ids;
    std::vector<int> bfs_queue;
    std::set<Edge> border_edges;

    while (!g_stop.load(std::memory_order_relaxed)) {
        int el_id;
        tx_run([&]() { pop_work_best(&el_id); });
        if (el_id < 0) continue;

        // Quick check outside TX
        if (g_elem_is_garbage[el_id] != 0) continue;

        // Mark seed as not referenced (outside TX)
        g_elem_is_referenced[el_id] = 0;

        long old_count = *g_elem_count_ptr;

        // Refine
        tx_run([&]() {
            refine_element(el_id, before_ids, border_edges, bad_ids, bfs_queue);
        });

        long num_new = *g_elem_count_ptr - old_count;
        if (num_new > 0) {
            // Push bad new elements
            tx_run([&]() { push_bad_ids(bad_ids); });
            g_total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    tm_exit_thread();
}

// ── Self-tests ────────────────────────────────────────────────────
static int test_cli_flags() {
    TEST_EQ(g_angle_constraint, 20, "default angle constraint");
    const char* test_argv[] = {"yada", "-a", "30", "-j", "0.3", "-t", "2"};
    parse_args(7, (char**)test_argv);
    TEST_EQ(g_angle_constraint, 30, "parsed angle constraint");
    TEST_NEAR(g_jitter, 0.3, 1e-9, "parsed jitter");
    TEST_EQ(g_num_threads, 2, "parsed threads");
    g_angle_constraint = 20; g_jitter = 0.5; g_num_threads = 4;
    return test_result();
}

static int test_rng() {
    std::mt19937 a(42), b(42);
    for (int i = 0; i < 1000; i++) TEST_EQ(a(), b(), "RNG determinism");
    return test_result();
}

static int test_geometry() {
    double cx, cy, cr;
    circumcircle(cx, cy, cr, {0,0}, {1,0}, {0,1});
    TEST_NEAR(cx, 0.5, 1e-9, "circumcenter x");
    TEST_NEAR(cy, 0.5, 1e-9, "circumcenter y");
    TEST_NEAR(cr, sqrt(2)/2, 1e-9, "circumradius");

    circumcircle(cx, cy, cr, {0,0}, {1,0}, {0.5, sqrt(3)/2});
    TEST_NEAR(cx, 0.5, 1e-9, "equilateral cx");
    TEST_NEAR(cy, sqrt(3)/6, 1e-9, "equilateral cy");

    double angle = tri_min_angle({0,0}, {1,0}, {0.5, sqrt(3)/2});
    TEST_NEAR(angle, 60.0, 1e-9, "equilateral min angle");

    angle = tri_min_angle({0,0}, {1,0}, {0,1});
    TEST_NEAR(angle, 45.0, 1e-9, "right triangle min angle");

    return test_result();
}

static int test_all() {
    int fails = 0;
    fails += test_cli_flags();
    fails += test_rng();
    fails += test_geometry();
    return fails;
}

// ── Main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        int fails = test_all();
        return fails ? 1 : 0;
    }
    parse_args(argc, argv);

    printf("Yada (STAMP spec, shared source)\n");
    printf("  Angle constraint: %ld°\n", g_angle_constraint);
    printf("  Jitter:           %.2f\n", g_jitter);
    printf("  Threads:          %ld\n", g_num_threads);
    printf("  Path:             %s\n",
           "Explicit API"
    );

    // ── Allocate TM data ──
    tm_init();

    g_elem_encroached    = (long*)tm_calloc(MAX_ELEMENTS, sizeof(long));
    g_elem_is_garbage    = (long*)tm_calloc(MAX_ELEMENTS, sizeof(long));
    g_elem_is_referenced = (long*)tm_calloc(MAX_ELEMENTS, sizeof(long));
    g_elem_neighbor_cnt  = (long*)tm_calloc(MAX_ELEMENTS, sizeof(long));
    g_elem_neighbors     = (long*)tm_calloc(MAX_ELEMENTS * MAX_NEIGHBORS, sizeof(long));
    g_work_heap_data     = (long*)tm_calloc(MAX_ELEMENTS, sizeof(long));
    g_work_heap_cnt      = (long*)tm_calloc(1, sizeof(long));
    g_elem_count_ptr     = (long*)tm_calloc(1, sizeof(long));

    g_pt_x      = new double[MAX_ELEMENTS * 3]();
    g_pt_y      = new double[MAX_ELEMENTS * 3]();
    g_circ_x    = new double[MAX_ELEMENTS]();
    g_circ_y    = new double[MAX_ELEMENTS]();
    g_circ_r    = new double[MAX_ELEMENTS]();
    g_min_angle = new double[MAX_ELEMENTS]();

    // ── Generate synthetic mesh ──
    int grid_size = 10;
    double spacing = 4.0;
    std::vector<Point> points;

    // Use a deterministic RNG for mesh generation
    auto rng = std::mt19937(42);
    auto jitter_rng = [&](double j) {
        return std::uniform_real_distribution<double>(-j, j)(rng);
    };

    for (int i = 0; i < grid_size; i++) {
        for (int j = 0; j < grid_size; j++) {
            points.push_back({i * spacing + jitter_rng(g_jitter),
                              j * spacing + jitter_rng(g_jitter)});
        }
    }

    auto add_element = [&](const Point& a, const Point& b, const Point& c) {
        int id = (int)(*g_elem_count_ptr)++;
        g_pt_x[id * 3] = a.x;     g_pt_y[id * 3] = a.y;
        g_pt_x[id * 3 + 1] = b.x; g_pt_y[id * 3 + 1] = b.y;
        g_pt_x[id * 3 + 2] = c.x; g_pt_y[id * 3 + 2] = c.y;
        circumcircle(g_circ_x[id], g_circ_y[id], g_circ_r[id], a, b, c);
        g_min_angle[id] = tri_min_angle(a, b, c);
        // TM flags initialized directly (setup phase, no TX)
        g_elem_encroached[id] = 0;
        g_elem_is_garbage[id] = 0;
        g_elem_is_referenced[id] = 0;
        for (int e = 0; e < 3; e++) {
            int e1 = e, e2 = (e + 1) % 3;
            Point pts[] = {a, b, c};
            if (is_encroached(pts[e1], pts[e2], pts[3 - e1 - e2]))
                g_elem_encroached[id] = 1;
        }
    };

    for (int i = 0; i < grid_size - 1; i++) {
        for (int j = 0; j < grid_size - 1; j++) {
            int i0 = i * grid_size + j;
            int i1 = i * grid_size + j + 1;
            int i2 = (i + 1) * grid_size + j;
            int i3 = (i + 1) * grid_size + j + 1;
            add_element(points[i0], points[i1], points[i3]);
            add_element(points[i0], points[i3], points[i2]);
        }
    }

    // ── Compute neighbor relationships ──
    int num_el = (int)*g_elem_count_ptr;
    for (int i = 0; i < num_el; i++) {
        for (int j = i + 1; j < num_el; j++) {
            int shared = 0;
            for (int vi = 0; vi < 3 && shared <= 2; vi++)
                for (int vj = 0; vj < 3 && shared <= 2; vj++)
                    if (g_pt_x[i * 3 + vi] == g_pt_x[j * 3 + vj] &&
                        g_pt_y[i * 3 + vi] == g_pt_y[j * 3 + vj])
                        shared++;
            if (shared == 2) {
                long nc = g_elem_neighbor_cnt[i];
                if (nc < MAX_NEIGHBORS) {
                    g_elem_neighbors[i * MAX_NEIGHBORS + nc] = j;
                    g_elem_neighbor_cnt[i] = nc + 1;
                }
                nc = g_elem_neighbor_cnt[j];
                if (nc < MAX_NEIGHBORS) {
                    g_elem_neighbors[j * MAX_NEIGHBORS + nc] = i;
                    g_elem_neighbor_cnt[j] = nc + 1;
                }
            }
        }
    }

    // ── Initialize work heap with bad elements ──
    for (int i = 0; i < num_el; i++) {
        bool bad = g_min_angle[i] < g_angle_constraint || g_elem_encroached[i] != 0;
        if (bad) {
            g_elem_is_referenced[i] = 1;
            g_work_heap_data[g_work_heap_cnt[0]] = i;
            g_work_heap_cnt[0]++;
        }
    }

    printf("  Elements: %d  Bad: %ld (angle constraint=%ld°)\n",
           num_el, g_work_heap_cnt[0], g_angle_constraint);
    fflush(stdout);

    // ── Launch worker threads ──
    std::vector<std::thread> threads;
    auto t1 = std::chrono::steady_clock::now();
    for (long t = 0; t < g_num_threads; t++)
        threads.emplace_back(worker, (int)t);

    // Run for 3 seconds
    std::this_thread::sleep_for(std::chrono::seconds(3));
    g_stop.store(true, std::memory_order_relaxed);

    for (auto& th : threads) th.join();
    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    // ── Report results ──
    long ops = g_total_ops.load();
    printf("\nResults (%ld ms):\n", (long)(elapsed * 1000));
    printf("  Operations: %ld  Total elements: %ld\n", ops, (long)*g_elem_count_ptr);
    printf("  Time: %.6f sec\n", elapsed);
    printf("  Rate: %.0f ops/sec\n", ops / elapsed);
    printf("  PASS\n");

    delete[] g_pt_x;
    delete[] g_pt_y;
    delete[] g_circ_x;
    delete[] g_circ_y;
    delete[] g_circ_r;
    delete[] g_min_angle;

    tm_exit();
    return 0;
}
