// Fuzz labyrinth — BFS path-finding + grid marking inside TX,
// mimicking STAMP labyrinth pattern. Invariant: after all TXs,
// each path cell marked -2 is reachable from its source.
//
// Build:
//   clang++ -std=c++20 -O0 -pthread -g -I$(PWD) \
//       -DTM_EVENT_LOG -DTM_BACKEND_TINYSTM -DDESIGN_WT \
//       -Ibackends/TinySTM -Ibackends \
//       tools/stm_bug_tool/benchmarks/fuzz_labyrinth.cpp \
//       backends/runtimes/TinySTM_runtime.cpp \
//       -o tools/stm_bug_tool/bin/fuzz_labyrinth_wt

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "backends/tm_event_logger.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;

void tm_init();
void tm_exit();
void tm_init_thread();
void tm_exit_thread();
void tm_begin();
void tm_end();

uint64_t  tm_read_i8(uint64_t *addr);
void tm_write_i8(uint64_t *addr, uint64_t val);
void* tm_malloc(size_t);
void  tm_free(void*);
}

template <typename F>
inline void tm_transaction(F&& body) {
    int committed = 0;
    while (!committed) {
        tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
        tm_begin();
        if (tm_longjmp_ret != 0)
            continue;
        body();
        tm_end();
        committed = 1;
    }
}

static int W, H, D, gridsize;
static long* g_grid;
static std::atomic<int> g_routed{0};

// BFS with ALL grid accesses through TM (mimicking plugin always-instrument)
static bool bfs_route_tm(int src, int dst) {
    long* buf = (long*)tm_malloc(gridsize * sizeof(long));

    // Copy grid to local buffer
    for (int i = 0; i < gridsize; i++)
        tm_write_i8((uint64_t*)&buf[i], tm_read_i8((uint64_t*)&g_grid[i]));

    int* queue = (int*)tm_malloc(gridsize * sizeof(int));
    int qh = 0, qt = 0;
    queue[qt++] = src;
    tm_write_i8((uint64_t*)&buf[src], 0);  // mark source

    int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    bool found = false;

    while (qh < qt) {
        int cur = queue[qh++];
        if (cur == dst) { found = true; break; }
        int cx = cur % W;
        int cy = (cur / W) % H;
        int cz = cur / (W * H);
        for (int d = 0; d < 6; d++) {
            int nx = cx + dirs[d][0], ny = cy + dirs[d][1], nz = cz + dirs[d][2];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H || nz < 0 || nz >= D) continue;
            int nidx = (nz * H + ny) * W + nx;
            long val = tm_read_i8((uint64_t*)&buf[nidx]);
            if (val == -2) continue;
            if (val == -1) {
                tm_write_i8((uint64_t*)&buf[nidx], 1);
                queue[qt++] = nidx;
            }
        }
    }

    if (found)
        tm_write_i8((uint64_t*)&g_grid[dst], -2);

    tm_free(queue);
    tm_free(buf);
    return found;
}

int main(int argc, char** argv) {
    int num_threads = argc > 1 ? atoi(argv[1]) : 1;
    int iters = argc > 2 ? atoi(argv[2]) : 5;
    W = argc > 3 ? atoi(argv[3]) : 5;
    H = W; D = W;
    gridsize = W * H * D;

    tm_init();
    TM_EVENT_INSTALL_SIGSEGV();

    g_grid = new long[gridsize];
    for (int i = 0; i < gridsize; i++) g_grid[i] = -1;

    int num_walls = gridsize / 8;
    unsigned seed = 42;
    for (int i = 0; i < num_walls; i++) {
        int idx = rand_r(&seed) % gridsize;
        if (g_grid[idx] == -1) g_grid[idx] = -2;
    }

    std::vector<int> srcs(iters), dsts(iters);
    for (int i = 0; i < iters; i++) {
        do { srcs[i] = rand_r(&seed) % gridsize; } while (g_grid[srcs[i]] == -2);
        do { dsts[i] = rand_r(&seed) % gridsize; } while (g_grid[dsts[i]] == -2 || dsts[i] == srcs[i]);
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([t, iters, num_threads, &srcs, &dsts]() {
            tm_init_thread();
            tm_nested_call_counter++;
            int local_routed = 0;

            for (int i = t; i < iters; i += num_threads) {
                tm_transaction([&]() {
                    if (bfs_route_tm(srcs[i], dsts[i]))
                        local_routed++;
                });
            }

            g_routed.fetch_add(local_routed, std::memory_order_relaxed);
            TM_EVENT_DUMP(0);
            tm_nested_call_counter--;
            tm_exit_thread();
        });
    }
    for (auto& th : threads) th.join();

    printf("INVARIANT: paths routed = %d / %d\n", g_routed.load(), iters);

    TM_EVENT_DUMP(0);
    tm_exit();
    delete[] g_grid;
    return 0;
}
