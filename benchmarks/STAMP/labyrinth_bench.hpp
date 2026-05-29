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

static bool do_expansion(long* grid_copy, const LabyrinthData* data,
                          const Point3D& src, const Point3D& dst,
                          int* queue, int& qh, int& qt) {
    int w = data->width, h = data->height, d = data->depth;
    int idx = grid_idx(data, src.x, src.y, src.z);
    grid_copy[idx] = 0;
    queue[qt++] = idx;

    int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

    while (qh < qt) {
        int cur = queue[qh++];
        int cx = cur % w;
        int cy = (cur / w) % h;
        int cz = cur / (w * h);
        
        if (cx == dst.x && cy == dst.y && cz == dst.z) {
            return true;
        }

        for (int dir = 0; dir < 6; dir++) {
            int nx = cx + dirs[dir][0];
            int ny = cy + dirs[dir][1];
            int nz = cz + dirs[dir][2];

            if (nx < 0 || nx >= w || ny < 0 || ny >= h || nz < 0 || nz >= d)
                continue;

            int nidx = (nz * h + ny) * w + nx;
            long val = grid_copy[nidx];
            if (val == -2L) continue;
            if (val == -1L) {
                grid_copy[nidx] = 1;
                queue[qt++] = nidx;
            }
        }
    }
    return false;
}

TX static bool labyrinth_route(LabyrinthData* data, int req_idx,
                                std::vector<long>& local_grid) {
    std::memcpy(local_grid.data(), data->grid.data(), data->grid.size() * sizeof(long));
    auto& req = data->requests[req_idx];
    
    int w = data->width, h = data->height, d = data->depth;
    int gridsize = w * h * d;
    
    // Allocate raw arrays (avoids std::queue/deque which break under TM)
    long* grid_copy = new long[gridsize];
    int* queue = new int[gridsize];
    for (int i = 0; i < gridsize; i++) grid_copy[i] = local_grid[i];
    
    int qh = 0, qt = 0;
    if (!do_expansion(grid_copy, data, req.src, req.dst, queue, qh, qt)) {
        delete[] grid_copy;
        delete[] queue;
        return false;
    }

    // Traceback using grid_copy's distance values
    // Simple greedy: at each step, move to the neighbor with lowest value
    std::vector<Point3D> path;
    int cur_x = req.dst.x, cur_y = req.dst.y, cur_z = req.dst.z;
    int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    
    while (true) {
        path.push_back({cur_x, cur_y, cur_z});
        int idx = (cur_z * h + cur_y) * w + cur_x;
        if (grid_copy[idx] == 0) break;
        
        int best_d = -1;
        long best_val = grid_copy[idx];
        
        for (int dir = 0; dir < 6; dir++) {
            int nx = cur_x + dirs[dir][0];
            int ny = cur_y + dirs[dir][1];
            int nz = cur_z + dirs[dir][2];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h || nz < 0 || nz >= d)
                continue;
            int nidx = (nz * h + ny) * w + nx;
            long nv = grid_copy[nidx];
            if (nv >= 0 && nv < best_val) {
                best_val = nv;
                best_d = dir;
            }
        }
        
        if (best_d < 0) {
            delete[] grid_copy;
            delete[] queue;
            path.clear();
            return false;
        }
        
        cur_x += dirs[best_d][0];
        cur_y += dirs[best_d][1];
        cur_z += dirs[best_d][2];
    }
    
    std::reverse(path.begin(), path.end());
    delete[] grid_copy;
    delete[] queue;

    // Mark path cells in the real grid
    for (size_t i = 1; i < path.size() - 1; i++) {
        int idx = grid_idx(data, path[i].x, path[i].y, path[i].z);
        if (data->grid[idx] != -1L) return false;
        data->grid[idx] = -2L;
    }

    return true;
}

THREAD void worker_labyrinth(ThreadData* td) {
    auto data = g_labyrinth;
    std::vector<long> local_grid(data->grid.size());

    for (int i = td->thread_id; i < data->num_requests; i += g_num_threads) {
        if (data->request_handled[i]) continue;

        bool ok = labyrinth_route(data, i, local_grid);
        if (ok) {
            data->request_handled[i] = 1;
            total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
