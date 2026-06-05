// STAMP/labyrinth benchmark — explicit TM API port
// Matches the plugin labyrinth_bench.hpp algorithm.

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <set>
#include <thread>
#include <vector>

// ── TM runtime declarations ────────────────────────────────────────
extern "C" {
    void     tm_init();
    void     tm_exit();
    void     tm_init_thread();
    void     tm_exit_thread();
    void*    tm_calloc(size_t n, size_t sz);
    long     tm_read_i8(const void* addr);
    void     tm_write_i8(void* addr, long val);
    void     tm_begin();
    void     tm_end();
    extern __thread int32_t tm_nested_call_counter;
    extern __thread int32_t tm_longjmp_ret;
    extern __thread sigjmp_buf tm_jmpbuf;
}

using PRNG = std::mt19937_64;

// ── TM helpers ────────────────────────────────────────────
static inline long tm_read_long(long* addr) {
    uint64_t raw = tm_read_i8(reinterpret_cast<uint64_t*>(addr));
    long val;
    memcpy(&val, &raw, sizeof(val));
    return val;
}

static inline void tm_write_long(long* addr, long val) {
    uint64_t raw;
    memcpy(&raw, &val, sizeof(raw));
    tm_write_i8(reinterpret_cast<uint64_t*>(addr), (int64_t)raw);
}

struct Point3D {
    int x, y, z;
    bool operator==(const Point3D& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Point3D& o) const { return !(*this == o); }
};

struct PathRequest {
    Point3D src;
    Point3D dst;
};

struct LabyrinthData {
    long* grid;            // flat array [width * height * depth]
    PathRequest* requests;
    int* request_handled;
    int width, height, depth;
    int num_requests;
};

static LabyrinthData g_data;

static inline int grid_idx(int w, int h, int x, int y, int z) {
    return (z * h + y) * w + x;
}

// ── BFS expansion (non-TX, works on local copy) ───────────
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
            if (cell_states[nidx] == -2L) continue;
            if (dist[nidx] == -1L) {
                dist[nidx] = dist[cur] + 1;
                queue[qt++] = nidx;
            }
        }
    }
    return 0;
}

// ── Greedy traceback (non-TX) ─────────────────────────────
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

// ── TX mark: atomically verify and mark path cells ────────
static bool labyrinth_mark(LabyrinthData* data, const std::vector<Point3D>& path) {
    bool result = false;
    tx_retry([&]() {
        int w = data->width, h = data->height, d = data->depth;
        // Verify all intermediate cells are free
        bool ok = true;
        for (size_t i = 1; i + 1 < path.size() && ok; i++) {
            int idx = grid_idx(w, h, path[i].x, path[i].y, path[i].z);
            if (tm_read_long(&data->grid[idx]) != -1L)
                ok = false;
        }
        if (!ok) { result = false; return; }
        // Mark them as blocked
        for (size_t i = 1; i + 1 < path.size(); i++) {
            int idx = grid_idx(w, h, path[i].x, path[i].y, path[i].z);
            tm_write_long(&data->grid[idx], -2L);
        }
        result = true;
    });
    return result;
}

// ── Worker thread ─────────────────────────────────────────
static std::atomic<uint64_t> g_total_ops{0};

static void worker(int thread_id, int num_threads) {
    expli::TM<int>::thread_init();

    auto* data = &g_data;
    int gridsize = data->width * data->height * data->depth;
    auto* local_grid = new long[gridsize];
    auto* dist = new long[gridsize];
    auto* queue = new int[gridsize];
    std::vector<Point3D> path;

    for (int i = thread_id; i < data->num_requests; i += num_threads) {
        if (data->request_handled[i]) continue;

        auto& req = data->requests[i];
        int w = data->width, h = data->height, d = data->depth;

        while (true) {
            // Snapshot grid state (non-TX — may be stale)
            memcpy(local_grid, data->grid, gridsize * sizeof(long));

            int ok = do_expansion(dist, local_grid, w, h, d,
                                   req.src, req.dst, queue);
            bool traced = ok && do_traceback(path, dist, w, h, d, req.src, req.dst);
            if (!ok || !traced || path.empty()) break;

            if (labyrinth_mark(data, path)) break;
            // TX failed — path cells taken. Retry.
        }

        data->request_handled[i] = 1;
        g_total_ops.fetch_add(1, std::memory_order_relaxed);
    }

    delete[] local_grid;
    delete[] dist;
    delete[] queue;
    expli::TM<int>::thread_exit();
}

int main(int argc, char* argv[]) {
    int num_threads = 4;
    int width = 8, height = 8, depth = 8;
    int num_requests = 64;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y") && i + 1 < argc) height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-z") && i + 1 < argc) depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) num_requests = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -x <width> -y <height> -z <depth> -n <requests>\n", argv[0]);
            return 0;
        }
    }

    expli::TM<int>::init();

    g_data.width = width;
    g_data.height = height;
    g_data.depth = depth;
    g_data.num_requests = num_requests;

    int gridsize = width * height * depth;
    g_data.grid = (long*)tm_malloc((size_t)gridsize * sizeof(long));
    std::fill(g_data.grid, g_data.grid + gridsize, -1L);

    PRNG rng(42);
    int num_walls = gridsize / 8;
    for (int i = 0; i < num_walls; i++) {
        int idx = (int)(rng() % gridsize);
        if (g_data.grid[idx] == -1L)
            g_data.grid[idx] = -2L;
    }

    g_data.requests = new PathRequest[num_requests];
    g_data.request_handled = new int[num_requests]();

    for (int i = 0; i < num_requests; i++) {
        int sx, sy, sz, dx, dy, dz;
        do {
            sx = (int)(rng() % width);
            sy = (int)(rng() % height);
            sz = (int)(rng() % depth);
        } while (g_data.grid[grid_idx(width, height, sx, sy, sz)] != -1L);
        do {
            dx = (int)(rng() % width);
            dy = (int)(rng() % height);
            dz = (int)(rng() % depth);
        } while (g_data.grid[grid_idx(width, height, dx, dy, dz)] != -1L ||
                 (dx == sx && dy == sy && dz == sz));
        g_data.requests[i] = {{sx, sy, sz}, {dx, dy, dz}};
    }

    printf("Maze size:    %ix%ix%i\n", width, height, depth);
    printf("Paths to route: %i\n", num_requests);
    fflush(stdout);

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++)
        threads.emplace_back(worker, i, num_threads);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    printf("    Time = %lld ms\n", (long long)elapsed);
    printf("    Routed = %llu / %d\n", (unsigned long long)ops, num_requests);

    delete[] g_data.requests;
    delete[] g_data.request_handled;
    expli::TM<int>::exit();
    return 0;
}
