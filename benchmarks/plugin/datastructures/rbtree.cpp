/**
 * Red-Black Tree Benchmark - Modern C++17 Version
 *
 * Tests transactional memory with concurrent Red-Black tree operations.
 * Uses a node pool with TM-annotated arrays for transactional access.
 *
 * Compiler: C++17
 * Uses: C++ Standard Library threads, std::atomic
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("shared"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int DEFAULT_INITIAL_SIZE = 1000;
constexpr int DEFAULT_RANGE_MAX = 10000;
constexpr int DEFAULT_READ_PCT = 80;
constexpr int DEFAULT_WRITE_PCT = 10;

constexpr int MAX_NODES = DEFAULT_RANGE_MAX * 2;

// Node pool with TM-annotated arrays
TM int node_keys[MAX_NODES];
TM int node_values[MAX_NODES];
TM bool node_red[MAX_NODES];
TM int node_left[MAX_NODES];
TM int node_right[MAX_NODES];
TM int node_parent[MAX_NODES];
TM int node_size[MAX_NODES];
TM int node_free[MAX_NODES];
TM int node_count = 0;
TM int root = -1;
TM int sentinel = -1;
TM int free_head = -1;

int max(int a, int b) {
    return a > b ? a : b;
}

int size(int n) {
    return n == sentinel ? 0 : node_size[n];
}

bool isRed(int n) {
    return n != sentinel && node_red[n];
}

int newNode(int key, int value) {
    int n;
    if (free_head != -1) {
        n = free_head;
        free_head = node_free[free_head];
    } else {
        n = node_count++;
        if (n >= MAX_NODES - 1) {
            fprintf(stderr, "FATAL: rbtree node pool exhausted (MAX_NODES=%d)\n", MAX_NODES);
            std::abort();
        }
    }
    node_keys[n] = key;
    node_values[n] = value;
    node_red[n] = true;
    node_left[n] = sentinel;
    node_right[n] = sentinel;
    node_parent[n] = sentinel;
    node_size[n] = 1;
    return n;
}

void freeNode(int n) {
    node_free[n] = free_head;
    free_head = n;
}

TM void leftRotate(int x) {
    int y = node_right[x];
    node_right[x] = node_left[y];
    if (node_left[y] != sentinel) {
        node_parent[node_left[y]] = x;
    }
    node_parent[y] = node_parent[x];
    if (node_parent[x] == sentinel) {
        root = y;
    } else if (x == node_left[node_parent[x]]) {
        node_left[node_parent[x]] = y;
    } else {
        node_right[node_parent[x]] = y;
    }
    node_left[y] = x;
    node_parent[x] = y;
    node_size[x] = size(node_left[x]) + size(node_right[x]) + 1;
    node_size[y] = size(node_left[y]) + size(node_right[y]) + 1;
}

TM void rightRotate(int y) {
    int x = node_left[y];
    node_left[y] = node_right[x];
    if (node_right[x] != sentinel) {
        node_parent[node_right[x]] = y;
    }
    node_parent[x] = node_parent[y];
    if (node_parent[y] == sentinel) {
        root = x;
    } else if (y == node_right[node_parent[y]]) {
        node_right[node_parent[y]] = x;
    } else {
        node_left[node_parent[y]] = x;
    }
    node_right[x] = y;
    node_parent[y] = x;
    node_size[y] = size(node_left[y]) + size(node_right[y]) + 1;
    node_size[x] = size(node_left[x]) + size(node_right[x]) + 1;
}

TM void insertFixup(int z) {
    while (node_red[node_parent[z]]) {
        if (node_parent[z] == node_left[node_parent[node_parent[z]]]) {
            int y = node_right[node_parent[node_parent[z]]];
            if (isRed(y)) {
                node_red[node_parent[z]] = false;
                node_red[y] = false;
                node_red[node_parent[node_parent[z]]] = true;
                z = node_parent[node_parent[z]];
            } else {
                if (z == node_right[node_parent[z]]) {
                    z = node_parent[z];
                    leftRotate(z);
                }
                node_red[node_parent[z]] = false;
                node_red[node_parent[node_parent[z]]] = true;
                rightRotate(node_parent[node_parent[z]]);
            }
        } else {
            int y = node_left[node_parent[node_parent[z]]];
            if (isRed(y)) {
                node_red[node_parent[z]] = false;
                node_red[y] = false;
                node_red[node_parent[node_parent[z]]] = true;
                z = node_parent[node_parent[z]];
            } else {
                if (z == node_left[node_parent[z]]) {
                    z = node_parent[z];
                    rightRotate(z);
                }
                node_red[node_parent[z]] = false;
                node_red[node_parent[node_parent[z]]] = true;
                leftRotate(node_parent[node_parent[z]]);
            }
        }
    }
    node_red[root] = false;
}

TM void insert(int key, int value) {
    int y = sentinel;
    int x = root;

    while (x != sentinel) {
        y = x;
        if (key < node_keys[x]) {
            x = node_left[x];
        } else if (key > node_keys[x]) {
            x = node_right[x];
        } else {
            node_values[x] = value;
            return;
        }
    }

    int z = newNode(key, value);
    node_parent[z] = y;
    if (y == sentinel) {
        root = z;
    } else if (key < node_keys[y]) {
        node_left[y] = z;
    } else {
        node_right[y] = z;
    }

    node_left[z] = sentinel;
    node_right[z] = sentinel;
    node_red[z] = true;
    node_size[z] = 1;

    // Update size on the path
    x = root;
    while (x != z) {
        node_size[x]++;
        if (key < node_keys[x])
            x = node_left[x];
        else
            x = node_right[x];
    }

    insertFixup(z);
}

TM void transplant(int u, int v) {
    if (node_parent[u] == sentinel) {
        root = v;
    } else if (u == node_left[node_parent[u]]) {
        node_left[node_parent[u]] = v;
    } else {
        node_right[node_parent[u]] = v;
    }
    node_parent[v] = node_parent[u];
}

TM int minimum(int n) {
    while (node_left[n] != sentinel) {
        n = node_left[n];
    }
    return n;
}

TM void eraseFixup(int x) {
    while (x != root && !isRed(x)) {
        if (x == node_left[node_parent[x]]) {
            int w = node_right[node_parent[x]];
            if (isRed(w)) {
                node_red[w] = false;
                node_red[node_parent[x]] = true;
                leftRotate(node_parent[x]);
                w = node_right[node_parent[x]];
            }
            if (!isRed(node_left[w]) && !isRed(node_right[w])) {
                node_red[w] = true;
                x = node_parent[x];
            } else {
                if (!isRed(node_right[w])) {
                    node_red[node_left[w]] = false;
                    node_red[w] = true;
                    rightRotate(w);
                    w = node_right[node_parent[x]];
                }
                node_red[w] = node_red[node_parent[x]];
                node_red[node_parent[x]] = false;
                node_red[node_right[w]] = false;
                leftRotate(node_parent[x]);
                x = root;
            }
        } else {
            int w = node_left[node_parent[x]];
            if (isRed(w)) {
                node_red[w] = false;
                node_red[node_parent[x]] = true;
                rightRotate(node_parent[x]);
                w = node_left[node_parent[x]];
            }
            if (!isRed(node_right[w]) && !isRed(node_left[w])) {
                node_red[w] = true;
                x = node_parent[x];
            } else {
                if (!isRed(node_left[w])) {
                    node_red[node_right[w]] = false;
                    node_red[w] = true;
                    leftRotate(w);
                    w = node_left[node_parent[x]];
                }
                node_red[w] = node_red[node_parent[x]];
                node_red[node_parent[x]] = false;
                node_red[node_left[w]] = false;
                rightRotate(node_parent[x]);
                x = root;
            }
        }
    }
    node_red[x] = false;
}

TM void erase(int key) {
    int z = root;
    while (z != sentinel) {
        if (key == node_keys[z]) {
            break;
        } else if (key < node_keys[z]) {
            z = node_left[z];
        } else {
            z = node_right[z];
        }
    }

    if (z == sentinel) {
        return;
    }

    int y = z;
    int x;
    bool y_original_red = node_red[y];

    if (node_left[z] == sentinel) {
        x = node_right[z];
        transplant(z, node_right[z]);
    } else if (node_right[z] == sentinel) {
        x = node_left[z];
        transplant(z, node_left[z]);
    } else {
        y = minimum(node_right[z]);
        y_original_red = node_red[y];
        x = node_right[y];
        if (node_parent[y] == z) {
            node_parent[x] = y;
        } else {
            transplant(y, node_right[y]);
            node_right[y] = node_right[z];
            node_parent[node_right[y]] = y;
        }
        transplant(z, y);
        node_left[y] = node_left[z];
        node_parent[node_left[y]] = y;
        node_red[y] = node_red[z];
        node_size[y] = node_size[z];
    }

    int to_update = node_parent[z];
    while (to_update != sentinel) {
        node_size[to_update]--;
        to_update = node_parent[to_update];
    }

    freeNode(z);

    if (!y_original_red) {
        eraseFixup(x);
    }
}

TM bool contains(int key) {
    int n = root;
    while (n != sentinel) {
        if (key == node_keys[n]) {
            return true;
        } else if (key < node_keys[n]) {
            n = node_left[n];
        } else {
            n = node_right[n];
        }
    }
    return false;
}

TM int get(int key, int default_val) {
    int n = root;
    while (n != sentinel) {
        if (key == node_keys[n]) {
            return node_values[n];
        } else if (key < node_keys[n]) {
            n = node_left[n];
        } else {
            n = node_right[n];
        }
    }
    return default_val;
}

TM int getRangeCount(int n, int minKey, int maxKey) {
    if (n == sentinel) {
        return 0;
    }
    int count = 0;
    if (node_keys[n] >= minKey && node_keys[n] <= maxKey) {
        count = 1;
    }
    if (node_keys[n] > minKey) {
        count += getRangeCount(node_left[n], minKey, maxKey);
    }
    if (node_keys[n] < maxKey) {
        count += getRangeCount(node_right[n], minKey, maxKey);
    }
    return count;
}

int treeSize() {
    return (root == sentinel) ? 0 : node_size[root];
}

TX void txn_insert(int key, int value) {
    insert(key, value);
}

TX void txn_erase(int key) {
    erase(key);
}

TX bool txn_contains(int key) {
    return contains(key);
}

TX int txn_get(int key, int default_val) {
    return get(key, default_val);
}

TX int txn_rangeCount(int minKey, int maxKey) {
    return getRangeCount(root, minKey, maxKey);
}

// Synchronization
class Barrier {
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
    int num_threads_;
    int crossing_;

public:
    explicit Barrier(int n) : count_(n), num_threads_(n), crossing_(0) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        crossing_++;
        if (crossing_ < num_threads_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

std::atomic<bool> stop_workers(false);

struct ThreadData {
    Barrier* barrier;
    std::atomic<uint64_t> nb_insert{0};
    std::atomic<uint64_t> nb_erase{0};
    std::atomic<uint64_t> nb_contains{0};
    std::atomic<uint64_t> nb_range{0};
    unsigned int seed;
    int thread_id;
    int read_pct;
    int write_pct;
    int range_max;
};

THREAD void worker(ThreadData* data) {
    std::mt19937 rng(data->seed);
    std::uniform_int_distribution<int> key_dist(0, data->range_max - 1);
    std::uniform_int_distribution<int> op_dist(0, 99);

    data->barrier->wait();

    while (!stop_workers.load(std::memory_order_relaxed)) {
        int op = op_dist(rng);

        if (op < data->write_pct) {
            int key = key_dist(rng);
            int value = key * 2;
            txn_insert(key, value);
            data->nb_insert.fetch_add(1, std::memory_order_relaxed);
        } else if (op < data->write_pct + data->read_pct) {
            int key = key_dist(rng);
            bool found = txn_contains(key);
            (void)found;
            data->nb_contains.fetch_add(1, std::memory_order_relaxed);
        } else if (op < data->write_pct + data->read_pct + 5) {
            int step = data->range_max / 10;
            if (step < 1) step = 1;
            int minKey = key_dist(rng) % step;
            int maxKey = minKey + step;
            int count = txn_rangeCount(minKey, maxKey);
            (void)count;
            data->nb_range.fetch_add(1, std::memory_order_relaxed);
        } else {
            int key = key_dist(rng);
            txn_erase(key);
            data->nb_erase.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

MAIN int main(int argc, char* argv[]) {
    int duration_ms = DEFAULT_DURATION_MS;
    int nb_threads = DEFAULT_NB_THREADS;
    int initial_size = DEFAULT_INITIAL_SIZE;
    int range_max = DEFAULT_RANGE_MAX;
    int read_pct = DEFAULT_READ_PCT;
    int write_pct = DEFAULT_WRITE_PCT;

    if (argc > 1) nb_threads = std::atoi(argv[1]);
    if (argc > 2) initial_size = std::atoi(argv[2]);
    if (argc > 3) duration_ms = std::atoi(argv[3]);
    if (argc > 4) read_pct = std::atoi(argv[4]);
    if (argc > 5) write_pct = std::atoi(argv[5]);
    if (argc > 6) range_max = std::atoi(argv[6]);

    // Validate arguments
    if (nb_threads < 1) { std::cerr << "ERROR: nb_threads must be >= 1\n"; return 1; }
    if (range_max < 10) { std::cerr << "ERROR: range_max must be >= 10\n"; return 1; }
    if (read_pct + write_pct > 95) { std::cerr << "ERROR: read_pct + write_pct must be <= 95\n"; return 1; }
    if (initial_size >= MAX_NODES) { std::cerr << "ERROR: initial_size too large for MAX_NODES=" << MAX_NODES << "\n"; return 1; }

    std::cout << "Red-Black Tree Benchmark\n"
              << "=====================\n"
              << "Threads:         " << nb_threads << "\n"
              << "Initial size:   " << initial_size << "\n"
              << "Duration:      " << duration_ms << " ms\n"
              << "Read %:        " << read_pct << "%\n"
              << "Write %:       " << write_pct << "%\n"
              << "Key range:      " << range_max << "\n"
              << std::endl;

    // Sentinel must be a valid array index (zero-initialized arrays are correct for a null node)
    sentinel = MAX_NODES - 1;
    root = sentinel;
    node_red[sentinel] = false;
    node_left[sentinel] = sentinel;
    node_right[sentinel] = sentinel;
    node_parent[sentinel] = sentinel;
    node_size[sentinel] = 0;

    std::cout << "Initial tree size: " << treeSize() << std::endl;

    Barrier barrier(nb_threads);
    std::vector<ThreadData> thread_data(nb_threads);
    std::vector<std::thread> threads;

    for (int i = 0; i < nb_threads; ++i) {
        thread_data[i].barrier = &barrier;
        thread_data[i].seed = i * 12345 + 42;
        thread_data[i].thread_id = i;
        thread_data[i].read_pct = read_pct;
        thread_data[i].write_pct = write_pct;
        thread_data[i].range_max = range_max;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < nb_threads; ++i) {
        threads.emplace_back(worker, &thread_data[i]);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    stop_workers.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    uint64_t total_inserts = 0, total_erases = 0, total_contains = 0, total_ranges = 0;
    for (const auto& td : thread_data) {
        total_inserts += td.nb_insert.load();
        total_erases += td.nb_erase.load();
        total_contains += td.nb_contains.load();
        total_ranges += td.nb_range.load();
    }

    uint64_t total_ops = total_inserts + total_erases + total_contains + total_ranges;
    int final_size = treeSize();

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed time:  " << elapsed_ms << " ms\n"
              << "Final size:    " << final_size << "\n"
              << "Total inserts: " << total_inserts << "\n"
              << "Total erases:   " << total_erases << "\n"
              << "Total reads:   " << (total_contains + total_ranges) << "\n"
              << "Total ops:     " << total_ops << "\n"
              << "Ops/sec:       " << (total_ops * 1000.0 / elapsed_ms) << "\n"
              << std::endl;

    return 0;
}