#include "../../../expli_tm_api/tm_api.hpp"
#include "../../../expli_tm_api/containers/tm_small_set.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <set>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    int angle = 20;
    double jitter = 0.5;
    unsigned seed = 42;
};

static Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i+1 < argc) c.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i+1 < argc) c.angle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-j") && i+1 < argc) c.jitter = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) c.seed = (unsigned)atoi(argv[++i]);
    }
    return c;
}

struct Point { double x, y; };

static bool operator<(const Point& a, const Point& b) {
    if (a.x != b.x) return a.x < b.x;
    return a.y < b.y;
}
static bool operator==(const Point& a, const Point& b) { return a.x == b.x && a.y == b.y; }

using Edge = std::pair<Point, Point>;

struct Element {
    Point pts[3];
    double circum_x, circum_y, circum_r;
    double min_angle;
    expli::TM<uint8_t> encroached;
    expli::TM<uint8_t> is_garbage;
    expli::TM<uint8_t> is_referenced;
    TMSmallSet<int, 8> neighbors;
};

struct YadaData {
    int max_elements;
    std::vector<Element> elements;
    std::vector<int> work_heap_data;
    expli::TM<int> work_heap_count;
    double angle_constraint;
};

struct Region {
    std::vector<int> before_ids;
    std::set<Edge> border_edges;
    std::vector<int> bad_ids;
};

// ── Geometry helpers (no TM needed — pure math) ──────────────
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
    if (b < a) return {b, a};
    return {a, b};
}

static double point_in_circum_sq(double cx, double cy, const Point& p) {
    return dist2({cx, cy}, p);
}

// ── Global state ────────────────────────────────────────────
static YadaData *g_data = nullptr;
static std::atomic<uint64_t> g_ops{0};
static std::atomic<bool> g_stop{false};

// ── Grow and retriangulate (TM-fields accessed via poke/peek) ─
static void grow_and_retriangulate(YadaData* data, int el_id, Region& region,
                                    std::vector<int>& bfs_queue) {
    bfs_queue.clear();
    region.before_ids.clear();
    region.border_edges.clear();
    region.bad_ids.clear();

    region.before_ids.push_back(el_id);
    bfs_queue.push_back(el_id);
    std::set<int> visited;
    visited.insert(el_id);
    size_t qidx = 0;

    double seed_cx = data->elements[el_id].circum_x;
    double seed_cy = data->elements[el_id].circum_y;
    double seed_cr = data->elements[el_id].circum_r;
    Point seed_p0 = data->elements[el_id].pts[0];
    Point seed_p1 = data->elements[el_id].pts[1];
    Point seed_p2 = data->elements[el_id].pts[2];
    double seed_cr_sq = seed_cr * seed_cr + 1e-10;

    while (qidx < bfs_queue.size()) {
        int cur_id = bfs_queue[qidx++];
        double cur_cx = data->elements[cur_id].circum_x;
        double cur_cy = data->elements[cur_id].circum_y;

        if (point_in_circum_sq(seed_cx, seed_cy, {cur_cx, cur_cy}) > seed_cr_sq) {
            for (int e = 0; e < 3; e++) {
                auto& el = data->elements[cur_id];
                Edge edge = make_edge(el.pts[e], el.pts[(e + 1) % 3]);
                region.border_edges.insert(edge);
            }
            continue;
        }

        int nc = data->elements[cur_id].neighbors.setup_count();
        for (int ni = 0; ni < nc; ni++) {
            int nid = data->elements[cur_id].neighbors.setup_get(ni);
            if (visited.find(nid) == visited.end()) {
                visited.insert(nid);
                bfs_queue.push_back(nid);
            }
        }
    }

    for (int bid : region.before_ids) {
        if (bid < data->max_elements)
            data->elements[bid].is_garbage.poke(1);
    }

    double cx = seed_p0.x / 3.0 + seed_p1.x / 3.0 + seed_p2.x / 3.0;
    double cy = seed_p0.y / 3.0 + seed_p1.y / 3.0 + seed_p2.y / 3.0;
    Point centroid = {cx, cy};
    int saved_new_id = (int)data->elements.size();

    int border_count = 0;
    for (auto& edge : region.border_edges) {
        if (border_count >= 3) break;
        Element tri;
        tri.pts[0] = centroid;
        tri.pts[1] = edge.first;
        tri.pts[2] = edge.second;
        circumcircle(tri.circum_x, tri.circum_y, tri.circum_r,
                     tri.pts[0], tri.pts[1], tri.pts[2]);
        tri.min_angle = tri_min_angle(tri.pts[0], tri.pts[1], tri.pts[2]);
        tri.encroached.poke(0);
        tri.is_garbage.poke(0);
        tri.is_referenced.poke(0);

        for (int e = 0; e < 3; e++) {
            int e1 = e, e2 = (e + 1) % 3;
            if (is_encroached(tri.pts[e1], tri.pts[e2], tri.pts[3 - e1 - e2]))
                tri.encroached.poke(1);
        }

        int tid = (int)data->elements.size();
        data->elements.push_back(tri);

        if ((tri.min_angle < data->angle_constraint || tri.encroached.peek()) && !tri.is_referenced.peek()) {
            tri.is_referenced.poke(1);
            data->elements[tid].is_referenced.poke(1);
            region.bad_ids.push_back(tid);
        }
        border_count++;
    }

    if (border_count > 0) {
        Element saved;
        saved.pts[0] = seed_p0;
        saved.pts[1] = seed_p1;
        saved.pts[2] = seed_p2;
        circumcircle(saved.circum_x, saved.circum_y, saved.circum_r,
                     centroid, seed_p0, seed_p1);
        saved.min_angle = 180.0;
        saved.encroached.poke(0);
        saved.is_garbage.poke(0);
        saved.is_referenced.poke(0);
        if (saved_new_id < (int)data->elements.size())
            data->elements[saved_new_id] = saved;
    }

    tx_retry([&]() {
        for (int bid : region.bad_ids) {
            if (!data->elements[bid].is_referenced.read()) {
                data->elements[bid].is_referenced.write(1);
                int n = data->work_heap_count.read();
                if (n < data->max_elements) {
                    data->work_heap_data[n] = bid;
                    data->work_heap_count.write(n + 1);
                }
            }
        }
    });
}

static void worker(int tid, const Config &cfg) {
    tm_init_thread();
    if (tid != 0) { tm_exit_thread(); return; }

    auto data = g_data;
    Region region;
    std::vector<int> bfs_queue;

    while (!g_stop.load(std::memory_order_relaxed)) {
        int el_id = -1;
        tx_retry([&]() {
            int n = data->work_heap_count.read();
            if (n == 0) return;
            int best = 0;
            uint8_t best_enc = data->elements[data->work_heap_data[0]].encroached.read();
            for (int i = 1; i < n; i++) {
                uint8_t enc = data->elements[data->work_heap_data[i]].encroached.read();
                if (enc > best_enc) { best_enc = enc; best = i; }
            }
            el_id = data->work_heap_data[best];
            data->work_heap_data[best] = data->work_heap_data[n - 1];
            data->work_heap_count.write(n - 1);
        });

        if (el_id < 0) continue;
        if (data->elements[el_id].is_garbage.peek()) continue;

        grow_and_retriangulate(data, el_id, region, bfs_queue);
        g_ops.fetch_add(1, std::memory_order_relaxed);
    }
    tm_exit_thread();
}

int main(int argc, char *argv[]) {
    auto cfg = parse_args(argc, argv);

    printf("Yada — Explicit TM API (TM-friendly data structures)\n");
    printf("Threads: %d  Angle: %d°  Jitter: %.2f\n",
           cfg.threads, cfg.angle, cfg.jitter);

    auto data = new YadaData();
    data->max_elements = 200000;
    data->elements.reserve(data->max_elements);
    data->work_heap_data.resize(data->max_elements, 0);
    data->work_heap_count.poke(0);
    data->angle_constraint = cfg.angle;

    auto rng = std::mt19937(cfg.seed);
    int grid_size = 10;
    double spacing = 4.0;
    std::vector<Point> points;

    for (int i = 0; i < grid_size; i++) {
        for (int j = 0; j < grid_size; j++) {
            double jx = std::uniform_real_distribution<double>(-cfg.jitter, cfg.jitter)(rng);
            double jy = std::uniform_real_distribution<double>(-cfg.jitter, cfg.jitter)(rng);
            points.push_back({i * spacing + jx, j * spacing + jy});
        }
    }

    auto add_element = [&](const Point& a, const Point& b, const Point& c) {
        Element el;
        el.pts[0] = a; el.pts[1] = b; el.pts[2] = c;
        circumcircle(el.circum_x, el.circum_y, el.circum_r, a, b, c);
        el.min_angle = tri_min_angle(a, b, c);
        el.encroached.poke(0);
        el.is_garbage.poke(0);
        el.is_referenced.poke(0);
        for (int e = 0; e < 3; e++) {
            int e1 = e, e2 = (e + 1) % 3;
            if (is_encroached(el.pts[e1], el.pts[e2], el.pts[3 - e1 - e2]))
                el.encroached.poke(1);
        }
        data->elements.push_back(el);
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

    int num_el = (int)data->elements.size();
    for (int i = 0; i < num_el; i++) {
        for (int j = i + 1; j < num_el; j++) {
            int shared = 0;
            for (int vi = 0; vi < 3 && shared <= 2; vi++)
                for (int vj = 0; vj < 3 && shared <= 2; vj++)
                    if (data->elements[i].pts[vi] == data->elements[j].pts[vj])
                        shared++;
            if (shared == 2) {
                data->elements[i].neighbors.setup_insert(j);
                data->elements[j].neighbors.setup_insert(i);
            }
        }
    }

    for (int i = 0; i < num_el; i++) {
        auto& el = data->elements[i];
        bool bad = el.min_angle < data->angle_constraint || el.encroached.peek();
        if (bad) {
            el.is_referenced.poke(1);
            int n = data->work_heap_count.peek();
            data->work_heap_data[n] = i;
            data->work_heap_count.poke(n + 1);
        }
    }

    printf("  Elements: %d  Bad: %d (angle constraint=%d°)\n",
           num_el, data->work_heap_count.peek(), cfg.angle);
    fflush(stdout);

    g_data = data;
    tm_init();

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; t++)
        threads.emplace_back(worker, t, std::ref(cfg));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    g_stop.store(true, std::memory_order_relaxed);
    for (auto &th : threads)
        th.join();
    tm_exit();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t ops = g_ops.load();
    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  Operations: %llu  Total elements: %zu\n",
           (unsigned long long)ops, data->elements.size());
    printf("  PASS\n");

    delete data;
    return 0;
}
