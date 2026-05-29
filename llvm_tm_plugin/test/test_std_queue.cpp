#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <vector>

#include "tm_test_common.hpp"

// ---- Repro: std::queue<ExpansionCell> BFS (Labyrinth pattern) ----
struct Cell {
    int x, y, z;
    long value;
};

static int g_queue_popped_x[1024];
static int g_queue_popped_y[1024];
static int g_queue_popped_z[1024];
static long g_queue_popped_v[1024];
static int g_queue_count = -1;
static int g_queue_ok = -1;

static int g_raw_popped_x[1024];
static int g_raw_popped_y[1024];
static int g_raw_popped_z[1024];
static long g_raw_popped_v[1024];
static int g_raw_count = -1;
static int g_raw_ok = -1;

TX void tx_bfs_queue() {
    // ---- Test 1: std::queue<Cell> BFS expansion ----
    std::queue<Cell> q;
    int written = 0;

    // Push 50 cells (simulating a short BFS)
    for (int i = 0; i < 50; i++) {
        q.push({i * 10, i * 10 + 1, i * 10 + 2, (long)(i * 100)});
        written++;
    }

    // Pop and verify FIFO order
    g_queue_count = 0;
    g_queue_ok = 1;
    for (int i = 0; i < written && i < 1024; i++) {
        Cell c = q.front(); q.pop();
        g_queue_popped_x[i] = c.x;
        g_queue_popped_y[i] = c.y;
        g_queue_popped_z[i] = c.z;
        g_queue_popped_v[i] = c.value;
        if (c.x != i * 10 || c.y != i * 10 + 1 || c.z != i * 10 + 2 ||
            c.value != (long)(i * 100)) {
            g_queue_ok = 0;
        }
        g_queue_count++;
    }

    // ---- Test 2: raw array BFS (same data, same order) ----
    Cell raw[64];
    int head = 0, tail = 0;

    for (int i = 0; i < 50; i++) {
        raw[tail++] = {i * 10, i * 10 + 1, i * 10 + 2, (long)(i * 100)};
    }

    g_raw_count = 0;
    g_raw_ok = 1;
    for (int i = 0; i < 50 && i < 1024; i++) {
        Cell c = raw[head++];
        g_raw_popped_x[i] = c.x;
        g_raw_popped_y[i] = c.y;
        g_raw_popped_z[i] = c.z;
        g_raw_popped_v[i] = c.value;
        if (c.x != i * 10 || c.y != i * 10 + 1 || c.z != i * 10 + 2 ||
            c.value != (long)(i * 100)) {
            g_raw_ok = 0;
        }
        g_raw_count++;
    }
}

MAIN int main() {
    tx_bfs_queue();

    fprintf(stderr, "std::queue<Cell> BFS:\n");
    fprintf(stderr, "  popped %d cells\n", g_queue_count);
    fprintf(stderr, "  ok: %s\n", g_queue_ok == 1 ? "YES" : "NO");
    fprintf(stderr, "  first cell: x=%d y=%d z=%d v=%ld (expected x=0 y=1 z=2 v=0)\n",
            g_queue_popped_x[0], g_queue_popped_y[0], g_queue_popped_z[0], g_queue_popped_v[0]);
    fprintf(stderr, "  last cell:  x=%d y=%d z=%d v=%ld (expected x=490 y=491 z=492 v=49000)\n",
            g_queue_popped_x[49], g_queue_popped_y[49], g_queue_popped_z[49], g_queue_popped_v[49]);
    // Find first wrong cell
    for (int i = 0; i < g_queue_count && i < 50; i++) {
        int ex = i * 10, ey = i * 10 + 1, ez = i * 10 + 2;
        long ev = (long)(i * 100);
        if (g_queue_popped_x[i] != ex || g_queue_popped_y[i] != ey ||
            g_queue_popped_z[i] != ez || g_queue_popped_v[i] != ev) {
            fprintf(stderr, "  WRONG at cell %d: got (%d,%d,%d,%ld) expected (%d,%d,%d,%ld)\n",
                    i, g_queue_popped_x[i], g_queue_popped_y[i], g_queue_popped_z[i],
                    g_queue_popped_v[i], ex, ey, ez, ev);
            break;
        }
    }

    fprintf(stderr, "raw array BFS:\n");
    fprintf(stderr, "  popped %d cells\n", g_raw_count);
    fprintf(stderr, "  ok: %s\n", g_raw_ok == 1 ? "YES" : "NO");
    fprintf(stderr, "  first cell: x=%d y=%d z=%d v=%ld (expected x=0 y=1 z=2 v=0)\n",
            g_raw_popped_x[0], g_raw_popped_y[0], g_raw_popped_z[0], g_raw_popped_v[0]);
    fprintf(stderr, "  last cell:  x=%d y=%d z=%d v=%ld (expected x=490 y=491 z=492 v=49000)\n",
            g_raw_popped_x[49], g_raw_popped_y[49], g_raw_popped_z[49], g_raw_popped_v[49]);

    if (g_queue_ok == 1 && g_raw_ok == 1) {
        printf("PASS: std::queue<Cell> and raw array both work\n");
        return 0;
    } else if (g_queue_ok != 1 && g_raw_ok == 1) {
        printf("FAIL: std::queue<Cell> broken but raw array works\n");
        return 1;
    } else if (g_queue_ok == 1 && g_raw_ok != 1) {
        printf("FAIL: std::queue<Cell> works but raw array broken\n");
        return 1;
    } else {
        printf("FAIL: both std::queue<Cell> and raw array broken\n");
        return 1;
    }
}
