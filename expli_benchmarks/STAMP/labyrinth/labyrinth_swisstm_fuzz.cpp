// Minimal labyrinth-style fuzz test for SwissTM.
// Pattern: inside a TX, allocate heap buffers, write to them (BFS simulation),
// read from shared TM grid, free buffers. This matches the LLVM plugin
// labyrinth_route() + do_expansion() pattern.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "../../../expli_tm_api/tm_api.hpp"

constexpr int GRID_SIZE = 125;  // 5x5x5

// Shared TM-tracked grid (like the labyrinth grid)
struct TM_GRID {
    TM_GRID() {
        for (int i = 0; i < GRID_SIZE; i++) {
            grid[i].poke(0L);
        }
    }
    expli::TM<int64_t> grid[GRID_SIZE];
};

static TM_GRID* g_tm_grid = nullptr;

// Fuzz-style test: single TX does BFS-like pattern on heap buffers
static void do_work(int thread_id, int iters) {
    g_tm_grid = new TM_GRID();
    auto& g = *g_tm_grid;

    expli::TM<int64_t>::init();
    expli::TM<int64_t>::thread_init();

    int ok = 0, fail = 0;
    for (int iter = 0; iter < iters; iter++) {
        expli::TM<int64_t>::begin();

        // Pattern 1: Allocate heap buffers (like grid_copy, queue)
        int64_t* buf1 = new int64_t[GRID_SIZE];
        int* buf2 = new int[GRID_SIZE];

        // Pattern 2: Write to heap buffers (like BFS expansion)
        for (int i = 0; i < GRID_SIZE; i++) {
            buf1[i] = (int64_t)i;
            buf2[i] = i;
        }

        // Pattern 3: Read from shared TM grid (like grid marking)
        int64_t sum = 0;
        for (int i = 0; i < GRID_SIZE; i++) {
            sum += g.grid[i].read();
        }

        // Pattern 4: Write to shared TM grid
        g.grid[0].write(42L);

        // Pattern 5: Read back from heap buffers and free
        int64_t check = 0;
        for (int i = 0; i < GRID_SIZE; i++) {
            check += buf1[i];
        }
        delete[] buf1;
        delete[] buf2;

        expli::TM<int64_t>::end();

        if (check == (int64_t)(GRID_SIZE - 1) * GRID_SIZE / 2 && sum == 0) {
            ok++;
        } else {
            printf("FAIL iter=%d check=%lld sum=%lld\n", iter, (long long)check, (long long)sum);
            fail++;
        }
    }

    printf("ok=%d fail=%d\n", ok, fail);
    expli::TM<int64_t>::thread_exit();
    expli::TM<int64_t>::exit();
}

int main() {
    do_work(0, 100);
    return 0;
}
