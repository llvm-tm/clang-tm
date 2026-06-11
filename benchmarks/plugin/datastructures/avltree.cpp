/**
 * AVL Tree Benchmark — Self-Balancing BST under TM
 * =================================================
 *
 * SPEC (standard AVL tree, Adelson-Velsky & Landis 1962):
 *   - Height-balanced binary search tree.
 *   - For every node, |height(left) − height(right)| ≤ 1.
 *   - Operations: insert(key, val), erase(key), contains(key), rangeCount(min, max).
 *   - Rebalancing via single/double rotations (rotateRight, rotateLeft).
 *
 * TM-specific:
 *   - Node pool is pre-allocated; TM annotations on node_keys/values/left/right.
 *   - txn_insert / txn_erase / txn_contains are the TX entry points.
 *   - Helper functions (height, getBalance, rotateRight, etc.) are cloned
 *     by the plugin for TM instrumentation.
 *   - Recursive helper functions (insert, erase, contains) add read-set entries
 *     for every accessed node.
 *
 * Workloads (80% reads / 10% writes / 10% inserts by default):
 *   - Transaction picks a random operation based on configured percentages,
 *     then calls the corresponding TX function.
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <stack>
#include "common.hpp"

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
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
TM int node_left[MAX_NODES];
TM int node_right[MAX_NODES];
TM int node_height[MAX_NODES];
TM int node_size[MAX_NODES];
TM int node_free[MAX_NODES];
TM int node_count = 0;
TM int root = 0;
TM int free_head = -1;

int max(int a, int b) {
    return a > b ? a : b;
}

__attribute__((noinline)) int height(int n) {
    if (n == -1) return 0;
    std::stack<int> st;
    st.push(n);
    int h = 0, max_h = 0;
    while (!st.empty()) {
        n = st.top(); st.pop();
        if (n == -1) { h--; continue; }
        h++;
        if (h > max_h) max_h = h;
        st.push(-1);   // marker: decrement height after children
        st.push(node_right[n]);
        st.push(node_left[n]);
    }
    return max_h;
}

int size(int n) {
    return (n == -1) ? 0 : node_size[n];
}

int newNode(int key, int value) {
    int n;
    if (free_head != -1) {
        n = free_head;
        free_head = node_free[free_head];
    } else {
        n = node_count++;
    }
    node_keys[n] = key;
    node_values[n] = value;
    node_left[n] = -1;
    node_right[n] = -1;
    node_height[n] = 1;
    node_size[n] = 1;
    return n;
}

void freeNode(int n) {
    node_free[n] = free_head;
    free_head = n;
}

TM int rotateRight(int y) {
    int x = node_left[y];
    int T2 = node_right[x];
    node_right[x] = y;
    node_left[y] = T2;
    node_height[y] = max(height(node_left[y]), height(node_right[y])) + 1;
    node_size[y] = size(node_left[y]) + size(node_right[y]) + 1;
    node_height[x] = max(height(node_left[x]), height(node_right[x])) + 1;
    node_size[x] = size(node_left[x]) + size(node_right[x]) + 1;
    return x;
}

TM int rotateLeft(int x) {
    int y = node_right[x];
    int T2 = node_left[y];
    node_left[y] = x;
    node_right[x] = T2;
    node_height[x] = max(height(node_left[x]), height(node_right[x])) + 1;
    node_size[x] = size(node_left[x]) + size(node_right[x]) + 1;
    node_height[y] = max(height(node_left[y]), height(node_right[y])) + 1;
    node_size[y] = size(node_left[y]) + size(node_right[y]) + 1;
    return y;
}

TM int getBalance(int n) {
    return (n == -1) ? 0 : (height(node_left[n]) - height(node_right[n]));
}

TM int insert(int node, int key, int value) {
    if (node == -1) {
        return newNode(key, value);
    }

    if (key < node_keys[node]) {
        node_left[node] = insert(node_left[node], key, value);
    } else if (key > node_keys[node]) {
        node_right[node] = insert(node_right[node], key, value);
    } else {
        node_values[node] = value;
        return node;
    }

    node_height[node] = max(height(node_left[node]), height(node_right[node])) + 1;
    node_size[node] = size(node_left[node]) + size(node_right[node]) + 1;

    int balance = getBalance(node);

    if (balance > 1 && key < node_keys[node_left[node]]) {
        return rotateRight(node);
    }
    if (balance < -1 && key > node_keys[node_right[node]]) {
        return rotateLeft(node);
    }
    if (balance > 1 && key > node_keys[node_left[node]]) {
        node_left[node] = rotateLeft(node_left[node]);
        return rotateRight(node);
    }
    if (balance < -1 && key < node_keys[node_right[node]]) {
        node_right[node] = rotateRight(node_right[node]);
        return rotateLeft(node);
    }

    return node;
}

TM int minValueNode(int node) {
    int current = node;
    while (node_left[current] != -1) {
        current = node_left[current];
    }
    return current;
}

TM int erase(int node, int key) {
    if (node == -1) {
        return -1;
    }

    if (key < node_keys[node]) {
        node_left[node] = erase(node_left[node], key);
    } else if (key > node_keys[node]) {
        node_right[node] = erase(node_right[node], key);
    } else {
        if (node_left[node] == -1 || node_right[node] == -1) {
            int temp = (node_left[node] != -1) ? node_left[node] : node_right[node];
            freeNode(node);
            return temp;
        } else {
            int temp = minValueNode(node_right[node]);
            node_keys[node] = node_keys[temp];
            node_values[node] = node_values[temp];
            node_right[node] = erase(node_right[node], node_keys[temp]);
        }
    }

    if (node == -1) {
        return -1;
    }

    node_height[node] = max(height(node_left[node]), height(node_right[node])) + 1;
    node_size[node] = size(node_left[node]) + size(node_right[node]) + 1;

    int balance = getBalance(node);

    if (balance > 1 && getBalance(node_left[node]) >= 0) {
        return rotateRight(node);
    }
    if (balance > 1 && getBalance(node_left[node]) < 0) {
        node_left[node] = rotateLeft(node_left[node]);
        return rotateRight(node);
    }
    if (balance < -1 && getBalance(node_right[node]) <= 0) {
        return rotateLeft(node);
    }
    if (balance < -1 && getBalance(node_right[node]) > 0) {
        node_right[node] = rotateRight(node_right[node]);
        return rotateLeft(node);
    }

    return node;
}

TM bool contains(int node, int key) {
    if (node == -1) {
        return false;
    }
    if (key < node_keys[node]) {
        return contains(node_left[node], key);
    }
    if (key > node_keys[node]) {
        return contains(node_right[node], key);
    }
    return true;
}

TM int get(int node, int key, int default_val) {
    if (node == -1) {
        return default_val;
    }
    if (key < node_keys[node]) {
        return get(node_left[node], key, default_val);
    }
    if (key > node_keys[node]) {
        return get(node_right[node], key, default_val);
    }
    return node_values[node];
}

TM int getRangeCount(int node, int minKey, int maxKey) {
    if (node == -1) {
        return 0;
    }
    int count = 0;
    if (node_keys[node] >= minKey && node_keys[node] <= maxKey) {
        count = 1;
    }
    if (node_keys[node] > minKey) {
        count += getRangeCount(node_left[node], minKey, maxKey);
    }
    if (node_keys[node] < maxKey) {
        count += getRangeCount(node_right[node], minKey, maxKey);
    }
    return count;
}

int treeSize() {
    return (root == -1) ? 0 : node_size[root];
}

TX void txn_insert(int key, int value) {
    root = insert(root, key, value);
}

TX void txn_erase(int key) {
    root = erase(root, key);
}

TX bool txn_contains(int key) {
    return contains(root, key);
}

TX int txn_get(int key, int default_val) {
    return get(root, key, default_val);
}

TX int txn_rangeCount(int minKey, int maxKey) {
    return getRangeCount(root, minKey, maxKey);
}

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

    std::cout << "AVL Tree Benchmark\n"
              << "=================\n"
              << "Threads:         " << nb_threads << "\n"
              << "Initial size:   " << initial_size << "\n"
              << "Duration:      " << duration_ms << " ms\n"
              << "Read %:        " << read_pct << "%\n"
              << "Write %:       " << write_pct << "%\n"
              << "Key range:      " << range_max << "\n"
              << std::endl;

    root = -1;

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