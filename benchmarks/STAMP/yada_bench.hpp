#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>
#include <vector>

struct YadaPoint {
    double x, y;
    bool operator<(const YadaPoint& o) const {
        if (x != o.x) return x < o.x;
        return y < o.y;
    }
    bool operator==(const YadaPoint& o) const { return x == o.x && y == o.y; }
};

using YadaEdge = std::pair<YadaPoint, YadaPoint>;

struct YadaElement {
    YadaPoint pts[3];
    double circum_x, circum_y, circum_r;
    double min_angle;
    bool encroached;
    bool is_garbage;
    bool is_referenced;
    YadaEdge encroached_edge;
    int encroached_idx;
    std::set<int> neighbors;
};

struct Region {
    std::vector<int> before_ids;
    std::set<YadaEdge> border_edges;
    std::vector<int> bad_ids;
};

struct TM YadaData {
    std::vector<YadaElement> elements;
    std::vector<int> work_heap;
    std::set<YadaEdge> boundary_set;
    double angle_constraint;
    int num_elements;
    int total_added;
};

static YadaData* g_yada = nullptr;

static double point_dist2(const YadaPoint& a, const YadaPoint& b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static void circumcircle_center(double& cx, double& cy, double& cr,
                                 const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
    double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(d) < 1e-15) { cr = 1e15; cx = cy = 0; return; }
    cx = ((a.x * a.x + a.y * a.y) * (b.y - c.y) +
          (b.x * b.x + b.y * b.y) * (c.y - a.y) +
          (c.x * c.x + c.y * c.y) * (a.y - b.y)) / d;
    cy = ((a.x * a.x + a.y * a.y) * (c.x - b.x) +
          (b.x * b.x + b.y * b.y) * (a.x - c.x) +
          (c.x * c.x + c.y * c.y) * (b.x - a.x)) / d;
    cr = std::sqrt(point_dist2({cx, cy}, a));
}

static double triangle_min_angle(const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
    auto angle = [](const YadaPoint& p, const YadaPoint& q, const YadaPoint& r) {
        double d1 = std::sqrt(point_dist2(p, q));
        double d2 = std::sqrt(point_dist2(p, r));
        double d3 = std::sqrt(point_dist2(q, r));
        if (d1 < 1e-15 || d2 < 1e-15) return 180.0;
        double dot = ((q.x - p.x) * (r.x - p.x) + (q.y - p.y) * (r.y - p.y)) / (d1 * d2);
        if (dot < -1.0) dot = -1.0;
        if (dot > 1.0) dot = 1.0;
        return std::acos(dot) * 180.0 / M_PI;
    };
    return std::min({angle(a, b, c), angle(b, a, c), angle(c, a, b)});
}

static bool is_encroached(const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
    auto midpoint = YadaPoint{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
    double r2 = point_dist2(a, b) / 4.0;
    return point_dist2(midpoint, c) <= r2;
}

static bool is_bad(const YadaElement& el, double angle_constraint) {
    return el.min_angle < angle_constraint || el.encroached;
}

inline void yada_generate_mesh() {
    auto data = new YadaData();
    data->angle_constraint = 20.0;
    data->total_added = 0;

    PRNG rng(42);
    std::vector<YadaPoint> points;
    int grid_size = 10;
    double spacing = 4.0;

    for (int i = 0; i < grid_size; i++) {
        for (int j = 0; j < grid_size; j++) {
            double px = i * spacing + rng.uniform(-0.5, 0.5);
            double py = j * spacing + rng.uniform(-0.5, 0.5);
            points.push_back({px, py});
        }
    }

    auto add_element = [&](const YadaPoint& a, const YadaPoint& b, const YadaPoint& c) {
        YadaElement el;
        el.pts[0] = a; el.pts[1] = b; el.pts[2] = c;
        circumcircle_center(el.circum_x, el.circum_y, el.circum_r, a, b, c);
        el.min_angle = triangle_min_angle(a, b, c);
        el.encroached = false;
        el.encroached_idx = -1;
        el.is_garbage = false;
        el.is_referenced = false;
        for (int e = 0; e < 3; e++) {
            int e1 = e, e2 = (e + 1) % 3;
            if (is_encroached(el.pts[e1], el.pts[e2], el.pts[3 - e1 - e2])) {
                el.encroached = true;
                el.encroached_idx = e;
                el.encroached_edge = {el.pts[e1], el.pts[e2]};
            }
        }
        data->elements.push_back(el);
    };

    for (int i = 0; i < grid_size - 1; i++) {
        for (int j = 0; j < grid_size - 1; j++) {
            int idx0 = i * grid_size + j;
            int idx1 = i * grid_size + j + 1;
            int idx2 = (i + 1) * grid_size + j;
            int idx3 = (i + 1) * grid_size + j + 1;
            add_element(points[idx0], points[idx1], points[idx3]);
            add_element(points[idx0], points[idx3], points[idx2]);
        }
    }
    data->num_elements = (int)data->elements.size();

    for (int i = 0; i < data->num_elements; i++) {
        for (int j = i + 1; j < data->num_elements; j++) {
            int shared = 0;
            for (int vi = 0; vi < 3; vi++) {
                for (int vj = 0; vj < 3; vj++) {
                    if (data->elements[i].pts[vi] == data->elements[j].pts[vj])
                        shared++;
                }
            }
            if (shared == 2) {
                data->elements[i].neighbors.insert(j);
                data->elements[j].neighbors.insert(i);
            }
        }
    }

    for (int i = 0; i < data->num_elements; i++) {
        for (int e = 0; e < 3; e++) {
            YadaEdge edge = {data->elements[i].pts[e], data->elements[i].pts[(e + 1) % 3]};
            if (edge.first < edge.second) std::swap(edge.first, edge.second);
            bool is_boundary = true;
            for (int n : data->elements[i].neighbors) {
                for (int ne = 0; ne < 3; ne++) {
                    YadaEdge ne_edge = {data->elements[n].pts[ne], data->elements[n].pts[(ne + 1) % 3]};
                    if (ne_edge.first < ne_edge.second) std::swap(ne_edge.first, ne_edge.second);
                    if (edge == ne_edge) {
                        is_boundary = false;
                        break;
                    }
                }
                if (!is_boundary) break;
            }
            if (is_boundary) {
                data->boundary_set.insert(edge);
            }
        }
    }

    for (int i = 0; i < data->num_elements; i++) {
        if (is_bad(data->elements[i], data->angle_constraint)) {
            data->elements[i].is_referenced = true;
            data->work_heap.push_back(i);
        }
    }

    auto heap_cmp = [data](int a, int b) {
        if (data->elements[a].encroached != data->elements[b].encroached)
            return data->elements[a].encroached < data->elements[b].encroached;
        return false;
    };
    std::make_heap(data->work_heap.begin(), data->work_heap.end(), heap_cmp);

    g_yada = data;
}

static YadaEdge make_sorted_edge(const YadaPoint& a, const YadaPoint& b) {
    if (b < a) return {b, a};
    return {a, b};
}

static bool point_in_circumcircle(const YadaElement& el, const YadaPoint& p) {
    double cx = el.circum_x, cy = el.circum_y;
    return point_dist2({cx, cy}, p) <= el.circum_r * el.circum_r + 1e-10;
}

TX static int yada_grow_region(YadaData* data, YadaElement& el, int el_id,
                                 Region& region, std::vector<int>& queue) {
    queue.clear();
    region.before_ids.clear();
    region.border_edges.clear();
    region.before_ids.push_back(el_id);
    queue.push_back(el_id);

    std::set<int> visited;
    visited.insert(el_id);
    size_t qidx = 0;

    while (qidx < queue.size()) {
        int cur_id = queue[qidx++];
        auto& cur = data->elements[cur_id];

        if (!point_in_circumcircle(el, {cur.circum_x, cur.circum_y})) {
            for (int e = 0; e < 3; e++) {
                YadaEdge edge = make_sorted_edge(cur.pts[e], cur.pts[(e + 1) % 3]);
                region.border_edges.insert(edge);
            }
            continue;
        }

        for (int nid : cur.neighbors) {
            if (visited.find(nid) == visited.end()) {
                visited.insert(nid);
                queue.push_back(nid);
            }
        }
    }

    return 0;
}

TX static bool yada_retriangulate(YadaData* data, int el_id, Region& region) {
    for (int bid : region.before_ids) {
        if (bid < data->num_elements) {
            data->elements[bid].is_garbage = true;
        }
    }

    int new_id = (int)data->elements.size();
    YadaElement new_el;
    new_el.pts[0] = data->elements[el_id].pts[0];
    new_el.pts[1] = data->elements[el_id].pts[1];
    new_el.pts[2] = data->elements[el_id].pts[2];

    double cx = 0, cy = 0;
    for (int i = 0; i < 3; i++) {
        cx += new_el.pts[i].x / 3.0;
        cy += new_el.pts[i].y / 3.0;
    }

    int border_count = 0;
    for (auto& edge : region.border_edges) {
        if (border_count >= 3) break;
        YadaElement tri;
        tri.pts[0] = {cx, cy};
        tri.pts[1] = edge.first;
        tri.pts[2] = edge.second;
        circumcircle_center(tri.circum_x, tri.circum_y, tri.circum_r, tri.pts[0], tri.pts[1], tri.pts[2]);
        tri.min_angle = triangle_min_angle(tri.pts[0], tri.pts[1], tri.pts[2]);
        tri.encroached = false;
        tri.encroached_idx = -1;
        tri.is_garbage = false;
        tri.is_referenced = false;

        for (int e = 0; e < 3; e++) {
            int e1 = e, e2 = (e + 1) % 3;
            if (is_encroached(tri.pts[e1], tri.pts[e2], tri.pts[3 - e1 - e2])) {
                tri.encroached = true;
                tri.encroached_idx = e;
            }
        }

        int tid = (int)data->elements.size();
        data->elements.push_back(tri);

        if (is_bad(tri, data->angle_constraint) && !tri.is_referenced) {
            tri.is_referenced = true;
            data->elements[tid].is_referenced = true;
            region.bad_ids.push_back(tid);
        }
        border_count++;
        data->total_added++;
    }

    data->elements[new_id] = new_el;
    return true;
}

THREAD void worker_yada(ThreadData* td) {
    auto data = g_yada;
    Region region;
    std::vector<int> bfs_queue;
    auto heap_cmp = [data](int a, int b) {
        if (data->elements[a].encroached != data->elements[b].encroached)
            return data->elements[a].encroached < data->elements[b].encroached;
        return false;
    };

    for (int iter = 0; iter < td->loops && !stop_workers; iter++) {
        int el_id = -1;

        if (!data->work_heap.empty()) {
            std::pop_heap(data->work_heap.begin(), data->work_heap.end(), heap_cmp);
            el_id = data->work_heap.back();
            data->work_heap.pop_back();
        }

        if (el_id < 0 || el_id >= (int)data->elements.size()) continue;
        auto& el = data->elements[el_id];

        if (el.is_garbage) continue;
        el.is_referenced = false;
        int old_num = (int)data->elements.size();

        yada_grow_region(data, el, el_id, region, bfs_queue);
        yada_retriangulate(data, el_id, region);

        int num_new = (int)data->elements.size() - old_num;
        if (num_new > 0) {
            total_ops.fetch_add(1, std::memory_order_relaxed);
        }

        region.before_ids.clear();
        region.border_edges.clear();
        for (int bid : region.bad_ids) {
            if (!data->elements[bid].is_referenced) {
                data->elements[bid].is_referenced = true;
                data->work_heap.push_back(bid);
                std::push_heap(data->work_heap.begin(), data->work_heap.end(), heap_cmp);
            }
        }
        region.bad_ids.clear();
    }
}
