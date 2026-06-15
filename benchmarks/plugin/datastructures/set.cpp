/**
 * Set Benchmark - Modern C++17 Version
 *
 * Tests transactional memory with concurrent set operations.
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

// Set using TM-annotated arrays (sorted array implementation)
TM int set_elements[MAX_NODES];
TM int set_size = 0;

TM int find(int value) {
    for (int i = 0; i < set_size; i++) {
        if (set_elements[i] == value) return i;
        if (set_elements[i] > value) break;
    }
    return -1;
}

TM bool contains(int value) {
    return find(value) >= 0;
}

TM bool insert(int value) {
    if (set_size >= MAX_NODES) return false;
    if (contains(value)) return false;
    
    // Find position to insert
    int pos = set_size;
    for (int i = set_size - 1; i >= 0; i--) {
        if (set_elements[i] < value) {
            pos = i + 1;
            break;
        }
        set_elements[i + 1] = set_elements[i];
    }
    set_elements[pos] = value;
    set_size++;
    return true;
}

TM bool remove(int value) {
    int pos = find(value);
    if (pos < 0) return false;
    
    // Shift elements
    for (int i = pos; i < set_size - 1; i++) {
        set_elements[i] = set_elements[i + 1];
    }
    set_size--;
    return true;
}

TM int size() {
    return set_size;
}

TX bool txn_insert(int value) {
    return insert(value);
}

TX bool txn_remove(int value) {
    return remove(value);
}

TX bool txn_contains(int value) {
    return contains(value);
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
    std::atomic<uint64_t> nb_remove{0};
    std::atomic<uint64_t> nb_contains{0};
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
            txn_insert(key);
            data->nb_insert.fetch_add(1, std::memory_order_relaxed);
        } else if (op < data->write_pct + data->read_pct) {
            int key = key_dist(rng);
            bool found = txn_contains(key);
            (void)found;
            data->nb_contains.fetch_add(1, std::memory_order_relaxed);
        } else {
            int key = key_dist(rng);
            txn_remove(key);
            data->nb_remove.fetch_add(1, std::memory_order_relaxed);
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

    std::cout << "Set Benchmark\n"
              << "==============\n"
              << "Threads:         " << nb_threads << "\n"
              << "Initial size:   " << initial_size << "\n"
              << "Duration:      " << duration_ms << " ms\n"
              << "Read %:        " << read_pct << "%\n"
              << "Write %:       " << write_pct << "%\n"
              << "Key range:      " << range_max << "\n"
              << std::endl;

    // Initialize set with random values
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, range_max - 1);
    for (int i = 0; i < initial_size; i++) {
        txn_insert(key_dist(rng));
    }

    std::cout << "Initial set size: " << size() << std::endl;

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

    uint64_t total_inserts = 0, total_removes = 0, total_contains = 0;
    for (const auto& td : thread_data) {
        total_inserts += td.nb_insert.load();
        total_removes += td.nb_remove.load();
        total_contains += td.nb_contains.load();
    }

    uint64_t total_ops = total_inserts + total_removes + total_contains;
    int final_size = size();

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed time:  " << elapsed_ms << " ms\n"
              << "Final size:    " << final_size << "\n"
              << "Total inserts: " << total_inserts << "\n"
              << "Total removes:  " << total_removes << "\n"
              << "Total reads:   " << total_contains << "\n"
              << "Total ops:     " << total_ops << "\n"
              << "Ops/sec:       " << (total_ops * 1000.0 / elapsed_ms) << "\n"
              << std::endl;

    return 0;
}