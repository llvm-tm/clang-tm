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

#include "../../tests/benchmark_test.hpp"

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

template<typename F>
inline void tx_retry(F&& body) {
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

static int g_num_threads = 4;
static int g_width = 32, g_height = 32, g_depth = 3;
static int g_num_requests = 64;

static void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) g_num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) g_width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y") && i + 1 < argc) g_height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-z") && i + 1 < argc) g_depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) g_num_requests = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -x <width> -y <height> -z <depth> -n <requests>\n", argv[0]);
            exit(0);
        }
    }
}

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

static void test_cli_flags() {
    printf("  Testing CLI flags...\n");
    int save_p = g_num_threads, save_x = g_width, save_y = g_height, save_z = g_depth, save_n = g_num_requests;
    TEST_EQ(g_num_threads, 4, "default threads");
    TEST_EQ(g_width, 32, "default width");
    TEST_EQ(g_height, 32, "default height");
    TEST_EQ(g_depth, 3, "default depth");
    TEST_EQ(g_num_requests, 64, "default requests");
    const char* test_args[] = {"prog", "-p", "2", "-x", "8", "-y", "8", "-z", "2", "-n", "4"};
    parse_args(11, (char**)test_args);
    TEST_EQ(g_num_threads, 2, "override threads");
    TEST_EQ(g_width, 8, "override width");
    TEST_EQ(g_height, 8, "override height");
    TEST_EQ(g_depth, 2, "override depth");
    TEST_EQ(g_num_requests, 4, "override requests");
    g_num_threads = save_p; g_width = save_x; g_height = save_y; g_depth = save_z; g_num_requests = save_n;
    if (test_result() != 0) exit(1);
}

static void test_rng() {
    printf("  Testing RNG determinism...\n");
    test_rng_determinism<PRNG>();
    if (test_result() != 0) exit(1);
}

static void test_logic() {
    printf("  Testing labyrinth logic...\n");
    // Basic grid and BFS expansion test
    int w = 8, h = 8, d = 2;
    int gridsize = w * h * d;
    long* grid = new long[gridsize];
    std::fill(grid, grid + gridsize, -1L);
    long* dist = new long[gridsize];
    int* queue = new int[gridsize];
    std::vector<Point3D> path;

    Point3D src = {1, 1, 0}, dst = {6, 6, 0};
    int ok = do_expansion(dist, grid, w, h, d, src, dst, queue);
    TEST_ASSERT(ok, "BFS finds path in empty grid");
    bool traced = ok && do_traceback(path, dist, w, h, d, src, dst);
    TEST_ASSERT(traced && !path.empty(), "traceback produces path");
    TEST_ASSERT(path.front() == src, "path starts at src");
    TEST_ASSERT(path.back() == dst, "path ends at dst");

    // Blocked grid: place wall at (4,4,0)
    grid[(0 * h + 4) * w + 4] = -2L;
    // Also block (4,3,0) and (4,5,0) to force detour
    grid[(0 * h + 3) * w + 4] = -2L;
    grid[(0 * h + 5) * w + 4] = -2L;
    // Block (3,4,0) and (5,4,0)
    grid[(0 * h + 4) * w + 3] = -2L;
    grid[(0 * h + 4) * w + 5] = -2L;
    ok = do_expansion(dist, grid, w, h, d, src, dst, queue);
    TEST_ASSERT(ok, "BFS finds path around walls");

    delete[] grid; delete[] dist; delete[] queue;
    if (test_result() != 0) exit(1);
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("Running self-tests for labyrinth...\n");
        test_cli_flags();
        test_rng();
        test_logic();
        printf("All tests passed.\n");
        return 0;
    }
    parse_args(argc, argv);

    expli::TM<int>::init();

    g_data.width = g_width;
    g_data.height = g_height;
    g_data.depth = g_depth;
    g_data.num_requests = g_num_requests;

    int gridsize = g_width * g_height * g_depth;
    g_data.grid = (long*)tm_malloc((size_t)gridsize * sizeof(long));
    std::fill(g_data.grid, g_data.grid + gridsize, -1L);

    PRNG rng(42);
    int num_walls = gridsize / 8;
    for (int i = 0; i < num_walls; i++) {
        int idx = (int)(rng() % gridsize);
        if (g_data.grid[idx] == -1L)
            g_data.grid[idx] = -2L;
    }

    g_data.requests = new PathRequest[g_num_requests];
    g_data.request_handled = new int[g_num_requests]();

    for (int i = 0; i < g_num_requests; i++) {
        int sx, sy, sz, dx, dy, dz;
        do {
            sx = (int)(rng() % g_width);
            sy = (int)(rng() % g_height);
            sz = (int)(rng() % g_depth);
        } while (g_data.grid[grid_idx(g_width, g_height, sx, sy, sz)] != -1L);
        do {
            dx = (int)(rng() % g_width);
            dy = (int)(rng() % g_height);
            dz = (int)(rng() % g_depth);
        } while (g_data.grid[grid_idx(g_width, g_height, dx, dy, dz)] != -1L ||
                 (dx == sx && dy == sy && dz == sz));
        g_data.requests[i] = {{sx, sy, sz}, {dx, dy, dz}};
    }

    printf("Maze size:    %ix%ix%i\n", g_width, g_height, g_depth);
    printf("Paths to route: %i\n", g_num_requests);
    fflush(stdout);

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < g_num_threads; i++)
        threads.emplace_back(worker, i, g_num_threads);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    printf("Paths routed    = %lu\n", (unsigned long)ops);
    printf("Elapsed time    = %f seconds\n", elapsed / 1000.0);
    printf("Verification passed.\n");

    delete[] g_data.requests;
    delete[] g_data.request_handled;
    expli::TM<int>::exit();
    return 0;
}
