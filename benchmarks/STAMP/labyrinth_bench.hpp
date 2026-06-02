#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <set>
#include <vector>

struct Point3D {
    int x, y, z;
    bool operator==(const Point3D& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Point3D& o) const { return !(*this == o); }
    bool operator<(const Point3D& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

struct PathRequest {
    Point3D src;
    Point3D dst;
};

struct TM LabyrinthData {
    std::vector<PathRequest> requests;
    std::vector<int> request_handled;
    std::vector<long> grid;
    int width, height, depth;
    int num_requests;
    long x_cost, y_cost, z_cost;
    long bend_cost;
};

struct ExpansionCell {
    int x, y, z;
    long value;
};

static LabyrinthData* g_labyrinth = nullptr;

inline void labyrinth_generate_maze() {
    auto data = new LabyrinthData();
    data->width = g_labyrinth_x;
    data->height = g_labyrinth_y;
    data->depth = g_labyrinth_z;
    data->x_cost = 1;
    data->y_cost = 1;
    data->z_cost = 2;
    data->bend_cost = 1;
    data->num_requests = g_labyrinth_n;

    int gridsize = data->width * data->height * data->depth;
    data->grid.resize(gridsize, -1L);

    PRNG rng(42);
    int num_walls = gridsize / 8;
    for (int i = 0; i < num_walls; i++) {
        int idx = (int)(rng.next() % gridsize);
        if (data->grid[idx] == -1L) {
            data->grid[idx] = -2L;
        }
    }

    data->requests.resize(data->num_requests);
    data->request_handled.resize(data->num_requests, 0);

    for (int i = 0; i < data->num_requests; i++) {
        int sx, sy, sz, dx, dy, dz;
        do {
            sx = (int)(rng.next() % data->width);
            sy = (int)(rng.next() % data->height);
            sz = (int)(rng.next() % data->depth);
        } while (data->grid[(sz * data->height + sy) * data->width + sx] != -1L);
        do {
            dx = (int)(rng.next() % data->width);
            dy = (int)(rng.next() % data->height);
            dz = (int)(rng.next() % data->depth);
        } while (data->grid[(dz * data->height + dy) * data->width + dx] != -1L ||
                 (dx == sx && dy == sy && dz == sz));

        data->requests[i] = {{sx, sy, sz}, {dx, dy, dz}};
    }

    g_labyrinth = data;

    printf("Maze size:    %ix%ix%i\n", data->width, data->height, data->depth);
    printf("Paths to route: %i\n", data->num_requests);
    fflush(stdout);
}

static inline int grid_idx(const LabyrinthData* data, int x, int y, int z) {
    return (z * data->height + y) * data->width + x;
}

// ── BFS expansion: computes distance field from src (non-TX, raw memory) ───
static int do_expansion(long* dist, const long* cell_states,
                         int w, int h, int d,
                         const Point3D& src, const Point3D& dst,
                         int* queue) {
    int gridsize = w * h * d;
    for (int i = 0; i < gridsize; i++) dist[i] = -1L;

    int qh = 0, qt = 0;
    int idx = (src.z * h + src.y) * w + src.x;
    dist[idx] = 0;
    queue[qt++] = idx;

    int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    while (qh < qt) {
        int cur = queue[qh++];
        int cx = cur % w, cy = (cur / w) % h, cz = cur / (w * h);
        if (cx == dst.x && cy == dst.y && cz == dst.z) return 1;

        for (int d2 = 0; d2 < 6; d2++) {
            int nx = cx + dirs[d2][0], ny = cy + dirs[d2][1], nz = cz + dirs[d2][2];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h || nz < 0 || nz >= d) continue;
            int nidx = (nz * h + ny) * w + nx;
            if (cell_states[nidx] == -2L) continue; // wall
            if (dist[nidx] == -1L) { // unvisited
                dist[nidx] = dist[cur] + 1;
                queue[qt++] = nidx;
            }
        }
    }
    return 0;
}

// ── Greedy traceback: reconstruct path from distance field ────────────────
static bool do_traceback(std::vector<Point3D>& path, const long* dist,
                          int w, int h, int d,
                          const Point3D& src, const Point3D& dst) {
    path.clear();
    int cx = dst.x, cy = dst.y, cz = dst.z;
    int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    while (true) {
        path.push_back({cx, cy, cz});
        int idx = (cz * h + cy) * w + cx;
        if (idx == (src.z * h + src.y) * w + src.x) break;

        int best_d = -1;
        long best_val = dist[idx];
        for (int d2 = 0; d2 < 6; d2++) {
            int nx = cx + dirs[d2][0], ny = cy + dirs[d2][1], nz = cz + dirs[d2][2];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h || nz < 0 || nz >= d) continue;
            long nv = dist[(nz * h + ny) * w + nx];
            if (nv >= 0 && nv < best_val) { best_val = nv; best_d = d2; }
        }
        if (best_d < 0) { path.clear(); return false; }
        cx += dirs[best_d][0]; cy += dirs[best_d][1]; cz += dirs[best_d][2];
    }
    std::reverse(path.begin(), path.end());
    return !path.empty();
}

// ── TX wrapper: atomically verify and mark path cells ─────────────────────
TX static bool labyrinth_mark(LabyrinthData* data,
                               const std::vector<Point3D>& path) {
    for (size_t i = 1; i + 1 < path.size(); i++) {
        int idx = grid_idx(data, path[i].x, path[i].y, path[i].z);
        if (data->grid[idx] != -1L) return false;
    }
    for (size_t i = 1; i + 1 < path.size(); i++) {
        int idx = grid_idx(data, path[i].x, path[i].y, path[i].z);
        data->grid[idx] = -2L;
    }
    return true;
}

THREAD void worker_labyrinth(ThreadData* td) {
    auto data = g_labyrinth;
    int gridsize = data->width * data->height * data->depth;
    std::vector<long> local_grid(data->grid.size());
    long* dist = new long[gridsize];
    int* queue = new int[gridsize];
    std::vector<Point3D> path;

    for (int i = td->thread_id; i < data->num_requests; i += g_num_threads) {
        if (data->request_handled[i]) continue;

        auto& req = data->requests[i];
        int w = data->width, h = data->height, d = data->depth;

        while (true) {
            // Load grid state (non-TX raw memcpy — may be stale, that's OK)
            std::memcpy(local_grid.data(), data->grid.data(),
                        data->grid.size() * sizeof(long));

            int ok = do_expansion(dist, local_grid.data(), w, h, d,
                                   req.src, req.dst, queue);
            bool traced = ok && do_traceback(path, dist, w, h, d, req.src, req.dst);
            if (!ok || !traced || path.empty()) break;

            if (labyrinth_mark(data, path)) break;
            // TX returned false — path cells were taken between memcpy and TX.
            // Loop back to reload grid state.
        }

        data->request_handled[i] = 1;
        total_ops.fetch_add(1, std::memory_order_relaxed);
    }
    delete[] dist;
    delete[] queue;
}
