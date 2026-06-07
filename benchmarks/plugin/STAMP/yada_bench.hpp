#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <fstream>
#include <vector>

// For the plugin path, YADA_READ/YADA_WRITE are plain dereferences
// that the LLVM TM pass instruments inside TX functions.
#define YADA_READ(p)     (*(const long*)(p))
#define YADA_WRITE(p, v) (*(long*)(p) = (long)(v))

static const int YADA_MAX_NEIGHBORS = 16;
static const int YADA_MAX_ELEMENTS = 200000;
static const int YADA_MAX_EDGES = 20000;

struct YadaPoint {
    double x, y;
    bool operator<(const YadaPoint& o) const {
        if (x != o.x) return x < o.x;
        return y < o.y;
    }
    bool operator==(const YadaPoint& o) const { return x == o.x && y == o.y; }
};

struct YadaEdge {
    YadaPoint a, b;
};

struct YadaData {
    long* encroached;
    long* is_garbage;
    long* is_referenced;
    long* neighbor_cnt;
    long* neighbors;
    long* work_heap;
    long* work_heap_cnt;

    double* pt_x;
    double* pt_y;
    double* circ_x;
    double* circ_y;
    double* circ_r;
    double* min_angle;

    double angle_constraint;
    long elem_count;
};

TM static YadaData* g_yada = nullptr;

static double point_dist2(const YadaPoint& a, const YadaPoint& b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static void circumcircle_center(double& cx, double& cy, double& cr,
                                 const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
    double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if ((d < 0 ? -d : d) < 1e-15) { cr = 1e15; cx = cy = 0; return; }
    cx = ((a.x * a.x + a.y * a.y) * (b.y - c.y) +
          (b.x * b.x + b.y * b.y) * (c.y - a.y) +
          (c.x * c.x + c.y * c.y) * (a.y - b.y)) / d;
    cy = ((a.x * a.x + a.y * a.y) * (c.x - b.x) +
          (b.x * b.x + b.y * b.y) * (a.x - c.x) +
          (c.x * c.x + c.y * c.y) * (b.x - a.x)) / d;
    cr = tm_sqrt(point_dist2({cx, cy}, a));
}

static double triangle_min_angle(const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
    auto angle = [](const YadaPoint& p, const YadaPoint& q, const YadaPoint& r) {
        double d1 = tm_sqrt(point_dist2(p, q));
        double d2 = tm_sqrt(point_dist2(p, r));
        if (d1 < 1e-15 || d2 < 1e-15) return 180.0;
        double dot = ((q.x - p.x) * (r.x - p.x) + (q.y - p.y) * (r.y - p.y)) / (d1 * d2);
        if (dot < -1.0) dot = -1.0;
        if (dot > 1.0) dot = 1.0;
        return tm_acos(dot) * 180.0 / M_PI;
    };
    double a1 = angle(a, b, c);
    double a2 = angle(b, a, c);
    double a3 = angle(c, a, b);
    return (a1 < a2) ? (a1 < a3 ? a1 : a3) : (a2 < a3 ? a2 : a3);
}

static bool is_encroached(const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
    auto midpoint = YadaPoint{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
    double r2 = point_dist2(a, b) / 4.0;
    return point_dist2(midpoint, c) <= r2;
}

static YadaEdge make_sorted_edge(const YadaPoint& a, const YadaPoint& b) {
    if (b < a) return {b, a};
    return {a, b};
}

static bool edge_eq(const YadaEdge& x, const YadaEdge& y) {
    return x.a == y.a && x.b == y.b;
}

// ── Mesh generation (single-threaded init) ─────────────────

inline void yada_generate_mesh() {
    auto data = new YadaData();

    data->encroached    = (long*)tm_calloc(YADA_MAX_ELEMENTS, sizeof(long));
    data->is_garbage    = (long*)tm_calloc(YADA_MAX_ELEMENTS, sizeof(long));
    data->is_referenced = (long*)tm_calloc(YADA_MAX_ELEMENTS, sizeof(long));
    data->neighbor_cnt  = (long*)tm_calloc(YADA_MAX_ELEMENTS, sizeof(long));
    data->neighbors     = (long*)tm_calloc(YADA_MAX_ELEMENTS * YADA_MAX_NEIGHBORS, sizeof(long));
    data->work_heap     = (long*)tm_calloc(YADA_MAX_ELEMENTS, sizeof(long));
    data->work_heap_cnt = (long*)tm_calloc(1, sizeof(long));

    data->pt_x      = new double[YADA_MAX_ELEMENTS * 3]();
    data->pt_y      = new double[YADA_MAX_ELEMENTS * 3]();
    data->circ_x    = new double[YADA_MAX_ELEMENTS]();
    data->circ_y    = new double[YADA_MAX_ELEMENTS]();
    data->circ_r    = new double[YADA_MAX_ELEMENTS]();
    data->min_angle = new double[YADA_MAX_ELEMENTS]();

    data->angle_constraint = g_yada_angle;
    data->elem_count = 0;

    std::vector<YadaPoint> points;

    auto add_element = [&](const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
        long id = data->elem_count++;
        data->pt_x[id * 3] = a.x;     data->pt_y[id * 3] = a.y;
        data->pt_x[id * 3 + 1] = b.x; data->pt_y[id * 3 + 1] = b.y;
        data->pt_x[id * 3 + 2] = c.x; data->pt_y[id * 3 + 2] = c.y;
        circumcircle_center(data->circ_x[id], data->circ_y[id], data->circ_r[id], a, b, c);
        data->min_angle[id] = triangle_min_angle(a, b, c);
    };

    if (g_yada_i && g_yada_i[0] != '\0') {
        std::string prefix = g_yada_i;
        bool is_mesh = (prefix.rfind(".mesh") == prefix.size() - 5);
        bool is_node = (prefix.rfind(".node") == prefix.size() - 5);

        if (is_mesh) {
            std::ifstream f(prefix);
            if (!f.is_open()) { std::cerr << "Error: could not open " << prefix << "\n"; std::exit(1); }
            int nv = 0, nt = 0;
            f >> nv >> nt;
            for (int i = 0; i < nv; i++) { double x, y; f >> x >> y; points.push_back({x, y}); }
            for (int i = 0; i < nt; i++) { int v0, v1, v2; f >> v0 >> v1 >> v2;
                add_element(points[v0], points[v1], points[v2]); }
        } else {
            if (is_node) prefix = prefix.substr(0, prefix.size() - 5);
            std::ifstream fn(prefix + ".node");
            if (!fn.is_open()) { std::cerr << "Error: could not open " << prefix << ".node\n"; std::exit(1); }
            int nv = 0, dim = 2, na = 0, nm = 0;
            fn >> nv >> dim >> na >> nm;
            for (int i = 0; i < nv; i++) {
                int idx; double x, y; fn >> idx >> x >> y;
                for (int a = 0; a < na; a++) { double attr; fn >> attr; }
                if (nm) { int m; fn >> m; }
                points.push_back({x, y});
            }
            std::ifstream fe(prefix + ".ele");
            if (!fe.is_open()) { std::cerr << "Error: could not open " << prefix << ".ele\n"; std::exit(1); }
            int nt = 0, npt = 3, ea = 0;
            fe >> nt >> npt >> ea;
            for (int i = 0; i < nt; i++) {
                int idx, v0, v1, v2; fe >> idx >> v0 >> v1 >> v2;
                add_element(points[v0-1], points[v1-1], points[v2-1]);
                for (int a = 0; a < ea; a++) { double attr; fe >> attr; }
            }
        }
    } else {
        PRNG rng(42);
        int gs = 10;
        double sp = 4.0;
        for (int i = 0; i < gs; i++)
            for (int j = 0; j < gs; j++)
                points.push_back({i * sp + rng.uniform(-g_yada_jitter, g_yada_jitter),
                                  j * sp + rng.uniform(-g_yada_jitter, g_yada_jitter)});
        for (int i = 0; i < gs - 1; i++)
            for (int j = 0; j < gs - 1; j++) {
                int i0 = i * gs + j, i1 = i * gs + j + 1;
                int i2 = (i + 1) * gs + j, i3 = (i + 1) * gs + j + 1;
                add_element(points[i0], points[i1], points[i3]);
                add_element(points[i0], points[i3], points[i2]);
            }
    }

    // Compute neighbor relationships
    long ne = data->elem_count;
    for (int i = 0; i < ne; i++) {
        for (int j = i + 1; j < ne; j++) {
            int shared = 0;
            for (int vi = 0; vi < 3 && shared <= 2; vi++)
                for (int vj = 0; vj < 3 && shared <= 2; vj++)
                    if (data->pt_x[i * 3 + vi] == data->pt_x[j * 3 + vj] &&
                        data->pt_y[i * 3 + vi] == data->pt_y[j * 3 + vj])
                        shared++;
            if (shared == 2) {
                long nc = data->neighbor_cnt[i];
                if (nc < YADA_MAX_NEIGHBORS)
                    data->neighbors[i * YADA_MAX_NEIGHBORS + nc] = j;
                data->neighbor_cnt[i] = nc + 1;
                nc = data->neighbor_cnt[j];
                if (nc < YADA_MAX_NEIGHBORS)
                    data->neighbors[j * YADA_MAX_NEIGHBORS + nc] = i;
                data->neighbor_cnt[j] = nc + 1;
            }
        }
    }

    // Build initial work heap
    long whc = 0;
    for (int i = 0; i < ne; i++) {
        bool bad = data->min_angle[i] < data->angle_constraint;
        if (!bad) {
            for (int e = 0; e < 3 && !bad; e++) {
                YadaPoint pts[3] = {
                    {data->pt_x[i * 3], data->pt_y[i * 3]},
                    {data->pt_x[i * 3 + 1], data->pt_y[i * 3 + 1]},
                    {data->pt_x[i * 3 + 2], data->pt_y[i * 3 + 2]}
                };
                if (is_encroached(pts[e], pts[(e + 1) % 3], pts[3 - e - (e + 1) % 3]))
                    bad = true;
            }
        }
        if (bad) {
            data->is_referenced[i] = 1;
            data->work_heap[whc++] = i;
        }
    }
    data->work_heap_cnt[0] = whc;

    std::cout << "Yada mesh: " << ne << " elements, "
              << whc << " bad (angle constraint="
              << data->angle_constraint << " deg)\n";

    g_yada = data;
}

// ── TX: pop best element from work heap ─────────────────────

TX static int yada_pop_work_best() {
    long n = YADA_READ(g_yada->work_heap_cnt);
    if (n == 0) return -1;
    int best = 0;
    long best_id = YADA_READ(&g_yada->work_heap[0]);
    long best_enc = YADA_READ(&g_yada->encroached[best_id]);
    for (long i = 1; i < n; i++) {
        long id = YADA_READ(&g_yada->work_heap[i]);
        long enc = YADA_READ(&g_yada->encroached[id]);
        if (enc > best_enc) { best_enc = enc; best = (int)i; }
    }
    int out = (int)YADA_READ(&g_yada->work_heap[best]);
    long last = n - 1;
    if (best != last)
        YADA_WRITE(&g_yada->work_heap[best], YADA_READ(&g_yada->work_heap[last]));
    YADA_WRITE(g_yada->work_heap_cnt, last);
    return out;
}

// ── TX: push element ID to work heap ────────────────────────

TX static void yada_push_work(int id) {
    long n = YADA_READ(g_yada->work_heap_cnt);
    if (n >= YADA_MAX_ELEMENTS) return;
    YADA_WRITE(&g_yada->work_heap[n], id);
    YADA_WRITE(g_yada->work_heap_cnt, n + 1);
}

// ── TX: apply cavity removal and new element writes ─────────

TX __attribute__((annotate("tm_allow_opaque")))
static void yada_apply_refinement(int el_id, const int* cavity,
                                   int cavity_count, const YadaEdge* border,
                                   int border_count, const YadaPoint& centroid) {
    // Mark cavity elements as garbage
    for (int i = 0; i < cavity_count; i++)
        YADA_WRITE(&g_yada->is_garbage[cavity[i]], 1);

    // Create new elements from centroid to each border edge
    long base = YADA_READ(&g_yada->elem_count);
    for (int i = 0; i < border_count; i++) {
        int tid = (int)(base + i);
        if (tid >= YADA_MAX_ELEMENTS) break;

        g_yada->pt_x[tid * 3] = centroid.x;
        g_yada->pt_y[tid * 3] = centroid.y;
        g_yada->pt_x[tid * 3 + 1] = border[i].a.x;
        g_yada->pt_y[tid * 3 + 1] = border[i].a.y;
        g_yada->pt_x[tid * 3 + 2] = border[i].b.x;
        g_yada->pt_y[tid * 3 + 2] = border[i].b.y;

        circumcircle_center(g_yada->circ_x[tid], g_yada->circ_y[tid],
                            g_yada->circ_r[tid], centroid, border[i].a, border[i].b);
        g_yada->min_angle[tid] = triangle_min_angle(centroid, border[i].a, border[i].b);

        YADA_WRITE(&g_yada->encroached[tid], 0);
        YADA_WRITE(&g_yada->is_garbage[tid], 0);
        YADA_WRITE(&g_yada->is_referenced[tid], 0);
    }
    YADA_WRITE(&g_yada->elem_count, base + border_count);
}

// ── Worker thread ───────────────────────────────────────────

THREAD void worker_yada(ThreadData* td) {
    auto data = g_yada;

    // Thread-local scratch buffers (reused across iterations)
    std::vector<int> cavity;
    std::vector<int> bfs_queue;
    std::vector<int> bad_ids;
    std::vector<YadaEdge> all_edges;
    int* visited = new int[YADA_MAX_ELEMENTS]();
    int visit_gen = 0;
    int empty_count = 0;

    for (int iter = 0; iter < td->loops && !stop_workers; iter++) {
        // ── Pop best element (TX) ──
        int el_id = yada_pop_work_best();
        if (el_id < 0) {
            empty_count++;
            if (empty_count >= 3) break;  // Heap empty, no more work
            continue;
        }
        empty_count = 0;
        if (data->is_garbage[el_id] != 0) continue;

        data->is_referenced[el_id] = 0;
        long old_count = data->elem_count;

        // ── Find cavity (BFS, outside TX) ──
        double seed_cx = data->circ_x[el_id];
        double seed_cy = data->circ_y[el_id];
        double seed_cr_sq = data->circ_r[el_id] * data->circ_r[el_id] + 1e-10;
        YadaPoint p0 = {data->pt_x[el_id * 3], data->pt_y[el_id * 3]};
        YadaPoint p1 = {data->pt_x[el_id * 3 + 1], data->pt_y[el_id * 3 + 1]};
        YadaPoint p2 = {data->pt_x[el_id * 3 + 2], data->pt_y[el_id * 3 + 2]};

        cavity.clear();
        bfs_queue.clear();
        all_edges.clear();
        visit_gen++;
        int vg = visit_gen;

        cavity.push_back(el_id);
        bfs_queue.push_back(el_id);
        visited[el_id] = vg;

        size_t qidx = 0;
        while (qidx < bfs_queue.size()) {
            int cur_id = bfs_queue[qidx++];
            double cur_cx = data->circ_x[cur_id];
            double cur_cy = data->circ_y[cur_id];
            double dx = cur_cx - seed_cx;
            double dy = cur_cy - seed_cy;

            if (dx * dx + dy * dy > seed_cr_sq) {
                YadaPoint ca = {data->pt_x[cur_id * 3], data->pt_y[cur_id * 3]};
                YadaPoint cb = {data->pt_x[cur_id * 3 + 1], data->pt_y[cur_id * 3 + 1]};
                YadaPoint cc = {data->pt_x[cur_id * 3 + 2], data->pt_y[cur_id * 3 + 2]};
                all_edges.push_back(make_sorted_edge(ca, cb));
                all_edges.push_back(make_sorted_edge(cb, cc));
                all_edges.push_back(make_sorted_edge(cc, ca));
                continue;
            }

            long nc = data->neighbor_cnt[cur_id];
            for (long ni = 0; ni < nc; ni++) {
                int nid = (int)data->neighbors[cur_id * YADA_MAX_NEIGHBORS + ni];
                if (visited[nid] != vg) {
                    visited[nid] = vg;
                    bfs_queue.push_back(nid);
                    cavity.push_back(nid);
                }
            }
        }

        // ── Dedup border edges: cancel interior edges (those appearing twice) ──
        // Sort all_edges, then scan: keep edges that appear exactly once.
        std::sort(all_edges.begin(), all_edges.end(),
                  [](const YadaEdge& x, const YadaEdge& y) {
                      if (x.a.x != y.a.x) return x.a.x < y.a.x;
                      if (x.a.y != y.a.y) return x.a.y < y.a.y;
                      if (x.b.x != y.b.x) return x.b.x < y.b.x;
                      return x.b.y < y.b.y;
                  });

        std::vector<YadaEdge> border;
        size_t ae = 0;
        while (ae < all_edges.size()) {
            size_t start = ae;
            while (ae < all_edges.size() && edge_eq(all_edges[ae], all_edges[start]))
                ae++;
            size_t count = ae - start;
            if (count == 1)
                border.push_back(all_edges[start]);
        }

        if (border.empty()) continue;

        double cx = p0.x / 3.0 + p1.x / 3.0 + p2.x / 3.0;
        double cy = p0.y / 3.0 + p1.y / 3.0 + p2.y / 3.0;
        YadaPoint centroid = {cx, cy};

        // ── Apply refinement (TX with tm_allow_opaque) ──
        yada_apply_refinement(el_id, cavity.data(), (int)cavity.size(),
                              border.data(), (int)border.size(), centroid);

        long num_new = data->elem_count - old_count;
        if (num_new > 0) {
            // Check new elements and push bad ones to work heap
            long base = old_count;
            for (int i = 0; i < (int)num_new; i++) {
                int tid = (int)(base + i);
                if (tid >= YADA_MAX_ELEMENTS) break;
                YadaPoint ta = {data->pt_x[tid * 3], data->pt_y[tid * 3]};
                YadaPoint tb = {data->pt_x[tid * 3 + 1], data->pt_y[tid * 3 + 1]};
                YadaPoint tc = {data->pt_x[tid * 3 + 2], data->pt_y[tid * 3 + 2]};
                bool enc = is_encroached(ta, tb, tc) ||
                           is_encroached(tb, tc, ta) ||
                           is_encroached(tc, ta, tb);
                bool bad = data->min_angle[tid] < data->angle_constraint || enc;
                if (bad) {
                    if (data->is_referenced[tid] == 0) {
                        data->is_referenced[tid] = 1;
                        yada_push_work(tid);
                    }
                }
            }
            total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    delete[] visited;
}
