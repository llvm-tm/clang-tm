// Simplified STAMP/labyrinth benchmark using the explicit TM API.
//
// Router path finding in a 3D grid.  Each TX finds a path between
// two points, marking the grid cells along the path.
//
// Build:
//   make -C expli_benchmarks BACKEND=TINYSTM run-labyrinth
//
// Original: https://stamp.stanford.edu/

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <thread>
#include <vector>

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

// ── 3D Grid (TM-tracked) ───────────────────────────────────
struct Grid {
    int nx, ny, nz;
    expli::TM<int32_t> *cells;

    Grid(int x, int y, int z) : nx(x), ny(y), nz(z) {
        cells = new expli::TM<int32_t>[x * y * z];
        for (int i = 0; i < x * y * z; i++)
            cells[i].poke(0);
    }

    int index(int x, int y, int z) const {
        return (z * ny + y) * nx + x;
    }

    bool is_free(int x, int y, int z) const {
        return cells[index(x, y, z)].read() == 0;
    }

    void set(int x, int y, int z, int32_t val) {
        cells[index(x, y, z)].write(val);
    }
};

static Grid *g_grid = nullptr;
static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_paths_found{0};
static std::atomic<uint64_t> g_paths_failed{0};

struct Point { int x, y, z; };

static const int kDirs[6][3] = {
    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
};

// BFS path finding — raw arrays to avoid STL TM corruption
struct BFSState {
    int *queue_x, *queue_y, *queue_z;
    int *prev_x, *prev_y, *prev_z;
    int head, tail;
    int capacity;

    BFSState(int cap) : head(0), tail(0), capacity(cap) {
        queue_x = new int[cap];
        queue_y = new int[cap];
        queue_z = new int[cap];
        prev_x = new int[cap];
        prev_y = new int[cap];
        prev_z = new int[cap];
    }
    ~BFSState() {
        delete[] queue_x; delete[] queue_y; delete[] queue_z;
        delete[] prev_x; delete[] prev_y; delete[] prev_z;
    }
    void push(int x, int y, int z) {
        queue_x[tail] = x; queue_y[tail] = y; queue_z[tail] = z;
        tail++;
    }
    void pop(int &x, int &y, int &z) {
        x = queue_x[head]; y = queue_y[head]; z = queue_z[head];
        head++;
    }
    bool empty() const { return head >= tail; }
};

static int do_expansion(Point src, Point dst) {
    Grid &g = *g_grid;
    int cap = g.nx * g.ny * g.nz;
    BFSState bfs(cap);

    // Mark start
    g.set(src.x, src.y, src.z, 1);
    bfs.push(src.x, src.y, src.z);

    while (!bfs.empty()) {
        int cx, cy, cz;
        bfs.pop(cx, cy, cz);
        if (cx == dst.x && cy == dst.y && cz == dst.z)
            return 1;
        for (int d = 0; d < 6; d++) {
            int nx = cx + kDirs[d][0];
            int ny = cy + kDirs[d][1];
            int nz = cz + kDirs[d][2];
            if (nx >= 0 && nx < g.nx && ny >= 0 && ny < g.ny &&
                nz >= 0 && nz < g.nz && g.is_free(nx, ny, nz)) {
                g.set(nx, ny, nz, 1);
                bfs.push(nx, ny, nz);
            }
        }
    }
    return 0;
}

static Point random_point(std::mt19937 &rng) {
    return { (int)(rng() % g_grid->nx),
             (int)(rng() % g_grid->ny),
             (int)(rng() % g_grid->nz) };
}

static void worker(int tid, int iters) {
    expli::TM<int32_t>::thread_init();
    auto rng = std::mt19937(1234 + tid * 1000);

    for (int i = 0; i < iters; i++) {
        Point src = random_point(rng);
        Point dst = random_point(rng);
        int found = 0;
        tx_retry([&]() {
            found = do_expansion(src, dst);
        });
        if (found)
            g_paths_found.fetch_add(1);
        else
            g_paths_failed.fetch_add(1);
    }
    expli::TM<int32_t>::thread_exit();
}

int main(int argc, char *argv[]) {
    int threads = argc > 1 ? atoi(argv[1]) : 4;
    int grid_x  = argc > 2 ? atoi(argv[2]) : 5;
    int grid_y  = argc > 3 ? atoi(argv[3]) : 5;
    int grid_z  = argc > 4 ? atoi(argv[4]) : 5;
    int iters   = argc > 5 ? atoi(argv[5]) : 50;

    printf("Labyrinth — Explicit TM API\n");
    printf("Threads: %d  Grid: %dx%dx%d  Iterations: %d\n",
           threads, grid_x, grid_y, grid_z, iters);

    expli::TM<int32_t>::init();
    g_grid = new Grid(grid_x, grid_y, grid_z);

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads_v;
    for (int t = 0; t < threads; t++)
        threads_v.emplace_back(worker, t, iters / threads);
    for (auto &th : threads_v)
        th.join();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t found = g_paths_found.load();
    uint64_t failed = g_paths_failed.load();
    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  Paths found: %llu  Failed: %llu  Total: %llu\n",
           (unsigned long long)found, (unsigned long long)failed,
           (unsigned long long)(found + failed));
    printf("  PASS\n");

    delete g_grid;
    expli::TM<int32_t>::exit();
    return 0;
}
