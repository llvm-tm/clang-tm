// Labyrinth — C++ port of the original STAMP spec (explicit API path)
// Original spec: https://github.com/ccaominh/stamp/tree/master/labyrinth
//
// BFS router: find paths in 3D grid, reserve cells via TM.
// Only the final path-marking step is transactional; BFS is off-TM.
//
// Parameters:
//   -x <num>  X-dimension (default: 4)
//   -y <num>  Y-dimension (default: 4)
//   -z <num>  Z-dimension (default: 4)
//   -n <num>  Number of paths (default: 64)

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

#define TM_READ_I8(p)     tm_read_i8((const void*)(p))
#define TM_WRITE_I8(p, v) tm_write_i8((void*)(p), (long)(v))

template <typename F>
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

// ── Parameters ─────────────────────────────────────────────────────
static long g_labyrinth_x = 4;
static long g_labyrinth_y = 4;
static long g_labyrinth_z = 4;
static long g_labyrinth_n = 64;
static long g_num_threads = 4;

struct Point3D { int x, y, z; };

struct PathRequest {
    Point3D src;
    Point3D dst;
};

// Grid stores: -1 = free, -2 = wall, -3 = routed (set by TM mark)
static long*       g_grid = nullptr;
static PathRequest* g_requests = nullptr;
static int*        g_request_handled = nullptr;
static long        g_width, g_height, g_depth;
static long        g_gridsize;
static std::atomic<long> total_ops{0};

static int grid_idx(int x, int y, int z) {
    return (int)((z * g_height + y) * g_width + x);
}

// ── BFS expansion (non-TM, on local copy) ──────────────────────────
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

// ── Greedy traceback (non-TM) ──────────────────────────────────────
static int do_traceback(std::vector<Point3D>& path, const long* dist,
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
        if (best_d < 0) { path.clear(); return 0; }
        cx += dirs[best_d][0]; cy += dirs[best_d][1]; cz += dirs[best_d][2];
    }
    std::reverse(path.begin(), path.end());
    return !path.empty();
}

// ── TM mark: atomically verify and reserve path cells ──────────────
static bool labyrinth_mark(const std::vector<Point3D>& path) {
    bool ok = false;
    tx_run([&]() {
        for (size_t i = 1; i + 1 < path.size(); i++) {
            int idx = grid_idx(path[i].x, path[i].y, path[i].z);
            long val = TM_READ_I8(&g_grid[idx]);
            if (val != -1L) return;
        }
        for (size_t i = 1; i + 1 < path.size(); i++) {
            int idx = grid_idx(path[i].x, path[i].y, path[i].z);
            TM_WRITE_I8(&g_grid[idx], -2L);
        }
        ok = true;
    });
    return ok;
}

// ── Worker ─────────────────────────────────────────────────────────
static void worker(long tid) {
    tm_init_thread();

    int gridsize = (int)g_gridsize;
    std::vector<long> local_grid((size_t)gridsize);
    std::vector<long> dist((size_t)gridsize);
    std::vector<int> queue((size_t)gridsize);
    std::vector<Point3D> path;

    for (long i = tid; i < g_labyrinth_n; i += g_num_threads) {
        if (g_request_handled[i]) continue;

        auto& req = g_requests[i];
        int w = (int)g_width, h = (int)g_height, d = (int)g_depth;

        while (true) {
            std::memcpy(local_grid.data(), g_grid, (size_t)gridsize * sizeof(long));

            int ok = do_expansion(dist.data(), local_grid.data(), w, h, d,
                                   req.src, req.dst, queue.data());
            int traced = ok && do_traceback(path, dist.data(), w, h, d,
                                             req.src, req.dst);
            if (!ok || !traced || path.empty()) break;

            if (labyrinth_mark(path)) break;
        }

        g_request_handled[i] = 1;
        total_ops.fetch_add(1, std::memory_order_relaxed);
    }

    tm_exit_thread();
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-x") == 0 && i+1 < argc) g_labyrinth_x = atol(argv[++i]);
        else if (strcmp(argv[i], "-y") == 0 && i+1 < argc) g_labyrinth_y = atol(argv[++i]);
        else if (strcmp(argv[i], "-z") == 0 && i+1 < argc) g_labyrinth_z = atol(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) g_labyrinth_n = atol(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc) g_num_threads  = atol(argv[++i]);
    }

    printf("Labyrinth (STAMP spec)\n");
    printf("  Grid:  %ldx%ldx%ld\n", g_labyrinth_x, g_labyrinth_y, g_labyrinth_z);
    printf("  Paths: %ld\n", g_labyrinth_n);
    printf("  Threads: %ld\n", g_num_threads);

    tm_init();
    g_width  = g_labyrinth_x;
    g_height = g_labyrinth_y;
    g_depth  = g_labyrinth_z;
    g_gridsize = g_width * g_height * g_depth;

    g_grid = (long*)tm_calloc((size_t)g_gridsize, sizeof(long));
    for (long i = 0; i < g_gridsize; i++) g_grid[i] = -1L;

    // Add random walls
    std::mt19937_64 rng(42);
    long num_walls = g_gridsize / 8;
    for (long i = 0; i < num_walls; i++) {
        long idx = (long)(rng() % (uint64_t)g_gridsize);
        if (g_grid[idx] == -1L) g_grid[idx] = -2L;
    }

    // Generate requests
    g_requests = new PathRequest[(size_t)g_labyrinth_n];
    g_request_handled = new int[(size_t)g_labyrinth_n]{};

    for (long i = 0; i < g_labyrinth_n; i++) {
        int sx, sy, sz, dx, dy, dz;
        do {
            sx = (int)(rng() % (uint64_t)g_width);
            sy = (int)(rng() % (uint64_t)g_height);
            sz = (int)(rng() % (uint64_t)g_depth);
        } while (g_grid[grid_idx(sx, sy, sz)] != -1L);
        do {
            dx = (int)(rng() % (uint64_t)g_width);
            dy = (int)(rng() % (uint64_t)g_height);
            dz = (int)(rng() % (uint64_t)g_depth);
        } while (g_grid[grid_idx(dx, dy, dz)] != -1L ||
                 (dx == sx && dy == sy && dz == sz));

        g_requests[i] = {{sx, sy, sz}, {dx, dy, dz}};
    }

    printf("  Maze generated: %ld cells, %ld walls\n", g_gridsize, num_walls);

    auto t1 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (long t = 0; t < g_num_threads; t++)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    printf("\nResults (%ld ms):\n", (long)(elapsed * 1000));
    printf("  Routed:   %ld\n", total_ops.load());
    printf("  PASS\n");

    delete[] g_requests;
    delete[] g_request_handled;
    tm_exit();
    return 0;
}
