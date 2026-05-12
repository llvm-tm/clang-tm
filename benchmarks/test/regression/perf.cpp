/**
 * Performance Regression Test - Modern C++17 Version
 *
 * Measures transactional memory read/write performance.
 * Tests various workload patterns: read-only, write-only, and mixed.
 *
 * Compiler: C++17
 * Uses: C++ Standard Library threads, std::atomic
 */

#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int DEFAULT_DURATION_MS = 5000;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int DEFAULT_ARRAY_SIZE = 1000;
constexpr int DEFAULT_READ_PCT = 100;

struct ArrayData {
    static constexpr int SIZE = DEFAULT_ARRAY_SIZE;
    TM uint64_t data[SIZE];
};

static ArrayData global_data;

struct Barrier {
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_ = 0;
    int num_threads_ = 0;
    int crossing_ = 0;

    explicit Barrier(int n) : num_threads_(n), count_(n) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        crossing_++;
        if (crossing_ < count_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

std::atomic<bool> stop_workers(false);

struct ThreadData {
    std::atomic<uint64_t> counter{0};
    Barrier* barrier;
    int thread_id;
    int nb_threads;
    int array_size;
    int read_pct;
    unsigned int seed;
};

THREAD void worker_read_only(ThreadData& data) {
    auto rng = std::mt19937(data.seed);
    std::uniform_int_distribution<> dist(0, data.array_size - 1);

    data.barrier->wait();

    uint64_t local_counter = 0;
    while (!stop_workers.load(std::memory_order_relaxed)) {
        int idx = dist(rng);
        volatile uint64_t val = global_data.data[idx];
        (void)val;
        local_counter++;
    }
    data.counter.store(local_counter);
}

THREAD void worker_read_write(ThreadData& data) {
    auto rng = std::mt19937(data.seed);
    std::uniform_int_distribution<> dist(0, data.array_size - 1);

    data.barrier->wait();

    uint64_t local_counter = 0;
    while (!stop_workers.load(std::memory_order_relaxed)) {
        int idx = dist(rng);
        if (data.read_pct >= 100 || (dist(rng) % 100) < data.read_pct) {
            volatile uint64_t val = global_data.data[idx];
            (void)val;
        } else {
            global_data.data[idx] = idx;
        }
        local_counter++;
    }
    data.counter.store(local_counter);
}

void worker_tx(ThreadData& data) {
    auto rng = std::mt19937(data.seed);
    std::uniform_int_distribution<> dist(0, data.array_size - 1);

    data.barrier->wait();

    uint64_t local_counter = 0;
    while (!stop_workers.load(std::memory_order_relaxed)) {
        int idx = dist(rng);
        if (data.read_pct >= 100 || (dist(rng) % 100) < data.read_pct) {
            volatile uint64_t val = global_data.data[idx];
            (void)val;
        } else {
            global_data.data[idx] = idx;
        }
        local_counter++;
    }
    data.counter.store(local_counter);
}

double getMedian(std::vector<double>& v) {
    if (v.empty()) return 0;
    size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + n, v.end());
    return v[n];
}

double getAverage(const std::vector<double>& v) {
    if (v.empty()) return 0;
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

MAIN int main(int argc, char* argv[]) {
    int duration_ms = DEFAULT_DURATION_MS;
    int nb_threads = DEFAULT_NB_THREADS;
    int array_size = DEFAULT_ARRAY_SIZE;
    int read_pct = DEFAULT_READ_PCT;
    bool use_tx = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-d" && i + 1 < argc) {
            duration_ms = std::stoi(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            nb_threads = std::stoi(argv[++i]);
        } else if (arg == "-s" && i + 1 < argc) {
            array_size = std::stoi(argv[++i]);
        } else if (arg == "-p" && i + 1 < argc) {
            read_pct = std::stoi(argv[++i]);
        } else if (arg == "--tx") {
            use_tx = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Perf Benchmark Usage:\n"
                      << "  -d <ms>       Duration in ms (default: " << DEFAULT_DURATION_MS << ")\n"
                      << "  -t <n>        Number of threads (default: " << DEFAULT_NB_THREADS << ")\n"
                      << "  -s <n>        Array size (default: " << DEFAULT_ARRAY_SIZE << ")\n"
                      << "  -p <pct>      Read percentage (default: " << DEFAULT_READ_PCT << ")\n"
                      << "  --tx          Use transactional mode\n";
            return 0;
        }
    }

    std::cout << "Perf Benchmark - Modern C++17 Version\n"
              << "========================================\n"
              << "Duration:  " << duration_ms << " ms\n"
              << "Threads:   " << nb_threads << "\n"
              << "Array:     " << array_size << " elements\n"
              << "Read %:    " << read_pct << "%\n"
              << "Mode:      " << (use_tx ? "Transactional" : "Non-transactional") << "\n"
              << std::endl;

    for (int i = 0; i < array_size; ++i) {
        global_data.data[i] = i;
    }

    Barrier barrier(nb_threads);
    std::vector<std::unique_ptr<ThreadData>> thread_data;
    std::vector<std::thread> threads;

    for (int i = 0; i < nb_threads; ++i) {
        auto data = std::make_unique<ThreadData>();
        data->barrier = &barrier;
        data->thread_id = i;
        data->nb_threads = nb_threads;
        data->array_size = array_size;
        data->read_pct = read_pct;
        data->seed = i + 1234;
        thread_data.push_back(std::move(data));
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    if (read_pct == 100) {
        for (int i = 0; i < nb_threads; ++i) {
            threads.emplace_back(worker_read_only, std::ref(*thread_data[i]));
        }
    } else {
        for (int i = 0; i < nb_threads; ++i) {
            threads.emplace_back(worker_read_write, std::ref(*thread_data[i]));
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    stop_workers.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    uint64_t total_ops = 0;
    for (const auto& data : thread_data) {
        total_ops += data->counter.load();
    }

    double ops_per_sec = (total_ops * 1000.0) / elapsed_ms;
    double ops_per_thread = total_ops / (double)nb_threads;

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed time: " << elapsed_ms << " ms\n"
              << "Total ops:    " << total_ops << "\n"
              << "Ops/sec:      " << ops_per_sec << "\n"
              << "Ops/sec/thread: " << ops_per_thread << "\n"
              << std::endl;

    return 0;
}
