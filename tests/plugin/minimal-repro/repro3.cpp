// Minimal labyrinth-like reproducer: vector<long> write inside TX
// Compares SingleGlobalLock (works) vs TinySTM WBCTL (crashes)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct TM LabyrinthData {
    std::vector<long> grid;
    int width, height, depth;
};

LabyrinthData* g_data = nullptr;

TX static void route(int req_idx, std::vector<long>& local_grid) {
    // Memcpy from global to local (like labyrinth_route)
    std::memcpy(local_grid.data(), g_data->grid.data(),
                g_data->grid.size() * sizeof(long));

    // Write to some grid cells
    int n = (int)g_data->grid.size();
    for (int i = 0; i < n; i++) {
        g_data->grid[i] = (long)(i * 2);
    }

    // Read back and verify
    for (int i = 0; i < n && i < 10; i++) {
        if (g_data->grid[i] != (long)(i * 2)) {
            fprintf(stderr, "MISMATCH at [%d]: got %ld, expected %ld\n",
                    i, g_data->grid[i], (long)(i * 2));
        }
    }
}

THREAD void worker(ThreadData* td) {
    std::vector<long> local_grid(g_data->grid.size());

    for (int i = td->thread_id; i < 1; i += g_num_threads) {
        route(i, local_grid);
    }
}

int main(int argc, char** argv) {
    int n = 125;
    if (argc > 1) n = atoi(argv[1]);

    g_data = new LabyrinthData();
    g_data->width = 5;
    g_data->height = 5;
    g_data->depth = 5;
    g_data->grid.resize(n, -1L);

    printf("Grid size: %zu elements (%zu bytes)\n",
           g_data->grid.size(), g_data->grid.size() * sizeof(long));

    run_benchmark(worker, 1);
    printf("Done.\n");
    return 0;
}
