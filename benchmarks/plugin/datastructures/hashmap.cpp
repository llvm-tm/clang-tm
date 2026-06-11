/**
 * Hash Map Benchmark — Open-Addressing Hash Table under TM
 * =========================================================
 *
 * SPEC (standard open-addressing hash table):
 *   - Fixed-capacity array of buckets, TM-annotated.
 *   - Operations: insert(key, val), erase(key), contains(key), get(key).
 *   - Linear probing on collision.
 *   - g_size tracks live entries; g_deleted_count tracks tombstones.
 *
 * TM-specific:
 *   - g_buckets, g_capacity, g_size, g_deleted_count are TM globals.
 *   - txn_insert / txn_erase / txn_contains / txn_get are TX entry points.
 *   - Helper functions (find, insert, erase) are cloned by the plugin.
 *   - Iterative algorithms (no recursion) — flat call graph.
 *
 * Workloads (80% reads / 10% writes / 10% inserts by default):
 *   - Same distribution as AVL tree for cross-comparison.
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <optional>
#include <array>

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
constexpr int HASHMAP_LOAD_FACTOR = 2;

// Hash Map Entry
struct HashEntry {
    int key;
    int value;
    HashEntry* next;
    bool deleted;

    HashEntry(int k, int v) : key(k), value(k), next(nullptr), deleted(false) {}
};

// Hash Map - using global arrays for TM annotation
constexpr int MAX_CAPACITY = DEFAULT_RANGE_MAX / HASHMAP_LOAD_FACTOR;

// Global TM data - these WILL be detected by the plugin
TM HashEntry* g_buckets[MAX_CAPACITY];
TM int g_capacity = MAX_CAPACITY;
TM int g_size = 0;
TM int g_deleted_count = 0;

static int hash(int key, int capacity) {
    unsigned int h = static_cast<unsigned int>(key);
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return static_cast<int>(h % capacity);
}

TM HashEntry* find(int key) {
    int idx = hash(key, g_capacity);
    HashEntry* e = g_buckets[idx];
    while (e) {
        if (!e->deleted && e->key == key) {
            return e;
        }
        e = e->next;
    }
    return nullptr;
}

TM void insert(int key, int value) {
    int idx = hash(key, g_capacity);
    HashEntry* prev = nullptr;
    HashEntry* e = g_buckets[idx];

    while (e) {
        if (e->key == key) {
            if (e->deleted) {
                e->deleted = false;
                e->value = value;
                g_size++;
                g_deleted_count--;
            } else {
                e->value = value;
            }
            return;
        }
        prev = e;
        e = e->next;
    }

    HashEntry* new_entry = new HashEntry(key, value);
    if (prev) {
        prev->next = new_entry;
    } else {
        g_buckets[idx] = new_entry;
    }
    g_size++;
}

TM bool erase(int key) {
    int idx = hash(key, g_capacity);
    HashEntry* prev = nullptr;
    HashEntry* e = g_buckets[idx];

    while (e) {
        if (e->key == key) {
            if (!e->deleted) {
                e->deleted = true;
                g_size--;
                g_deleted_count++;
            }
            return true;
        }
        prev = e;
        e = e->next;
    }
    return false;
}

TM bool contains(int key) {
    HashEntry* e = find(key);
    return e && !e->deleted;
}

TM int get(int key, int default_val) {
    HashEntry* e = find(key);
    if (e && !e->deleted) {
        return e->value;
    }
    return default_val;
}

TM int countRange(int minKey, int maxKey) {
    int count = 0;
    for (int i = 0; i < g_capacity; ++i) {
        HashEntry* e = g_buckets[i];
        while (e) {
            if (!e->deleted && e->key >= minKey && e->key <= maxKey) {
                count++;
            }
            e = e->next;
        }
    }
    return count;
}

int size() { return g_size; }

TX void txn_insert(int key, int value) {
    insert(key, value);
}

TX bool txn_erase(int key) {
    return erase(key);
}

TX bool txn_contains(int key) {
    return contains(key);
}

TX int txn_get(int key, int default_val = -1) {
    return get(key, default_val);
}

TX int rangeCount(int minKey, int maxKey) {
    return countRange(minKey, maxKey);
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
            int count = rangeCount(minKey, maxKey);
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

    std::cout << "Hash Map Benchmark\n"
              << "================\n"
              << "Threads:         " << nb_threads << "\n"
              << "Initial size:   " << initial_size << "\n"
              << "Duration:      " << duration_ms << " ms\n"
              << "Read %:        " << read_pct << "%\n"
              << "Write %:       " << write_pct << "%\n"
              << "Key range:      " << range_max << "\n"
              << std::endl;

    // Initialize map
    std::mt19937 init_rng(42);
    std::uniform_int_distribution<int> dist(0, range_max - 1);
    for (int i = 0; i < initial_size; ++i) {
        int key = dist(init_rng);
        insert(key, key);
    }

    std::cout << "Initial map size: " << size() << std::endl;

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
    int final_size = size();

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