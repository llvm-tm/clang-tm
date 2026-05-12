/**
 * YCSB Benchmark - Full Specification Implementation
 *
 * Based on: Benchmarking Cloud Serving Systems with YCSB
 * Authors: Brian F. Cooper, Adam Silberstein, Erwin Tam, Raghu Ramakrishnan, Russell Sears
 * Published: Yahoo! Research, 2010
 *
 * GitHub: https://github.com/brianfrankcooper/YCSB
 * Wiki: https://github.com/brianfrankcooper/YCSB/wiki
 *
 * Workloads:
 * - A: 50% read, 50% update (update-heavy)
 * - B: 95% read, 5% update (read-heavy)
 * - C: 100% read (read-only)
 * - D: 95% read, 5% insert (read latest)
 * - E: 95% read, 5% short range scans
 * - F: 50% read-modify-write (50% scan + 50% update)
 *
 * Distribution:
 * - Default: Zipfian (hotspots)
 * - Optional: Uniform, Latest
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <cstring>
#include <unordered_map>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int MAX_RECORDS = 100000;
constexpr int FIELD_SIZE = 100;
constexpr int NUM_FIELDS = 10;

constexpr double ZIPFIAN_CONSTANT = 0.99;
constexpr int ZIPFIAN_ITEMS = 10000;

static const char CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

struct Record {
    char key[20];
    char data[NUM_FIELDS][FIELD_SIZE];
    int timestamp;
};

TM Record g_records[MAX_RECORDS];
TM int g_record_count = 0;
TM int g_record_counter = 0;

TM std::unordered_map<int, int> g_key_to_index;

enum class Distribution { UNIFORM, ZIPFIAN, LATEST };
enum class WorkloadType { A, B, C, D, E, F };

Distribution g_distribution = Distribution::ZIPFIAN;

std::vector<double> g_zipfian_cdf;

static double zeta(int n, double theta) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += 1.0 / std::pow(i + 1, theta);
    }
    return sum;
}

static void init_zipfian(int n, double theta) {
    g_zipfian_cdf.resize(n);
    double z = zeta(n, theta);
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += 1.0 / std::pow(i + 1, theta);
        g_zipfian_cdf[i] = sum / z;
    }
}

static inline int zipfian_next(double r) {
    for (int i = 0; i < (int)g_zipfian_cdf.size(); i++) {
        if (r < g_zipfian_cdf[i]) return i;
    }
    return g_zipfian_cdf.size() - 1;
}

static inline int hash_key(int key) {
    unsigned int h = static_cast<unsigned int>(key);
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return static_cast<int>(h % MAX_RECORDS);
}

static void init_record(Record* rec, int key) {
    snprintf(rec->key, sizeof(rec->key), "user%09d", key);
    for (int f = 0; f < NUM_FIELDS; f++) {
        for (int i = 0; i < FIELD_SIZE - 1; i++) {
            rec->data[f][i] = CHARS[(key + f + i) % 62];
        }
        rec->data[f][FIELD_SIZE - 1] = '\0';
    }
    rec->timestamp = key;
}

TM bool exists(int key) {
    auto it = g_key_to_index.find(key);
    if (it == g_key_to_index.end()) return false;
    return g_records[it->second].key[0] != '\0';
}

TM void read(int key, char* out_data, int max_len) {
    auto it = g_key_to_index.find(key);
    if (it == g_key_to_index.end()) return;
    int idx = it->second;
    Record& rec = g_records[idx];
    int len = std::min(max_len - 1, (int)strlen(rec.data[0]));
    memcpy(out_data, rec.data[0], len);
    out_data[len] = '\0';
}

TM void read_all_fields(int key, char output[NUM_FIELDS][FIELD_SIZE]) {
    auto it = g_key_to_index.find(key);
    if (it == g_key_to_index.end()) return;
    int idx = it->second;
    Record& rec = g_records[idx];
    for (int f = 0; f < NUM_FIELDS; f++) {
        memcpy(output[f], rec.data[f], FIELD_SIZE);
    }
}

TM int scan(int start_key, int count, std::vector<int>& results) {
    int found = 0;
    for (int i = 0; i < count && found < 10; i++) {
        int key = start_key + i;
        auto it = g_key_to_index.find(key % MAX_RECORDS);
        if (it != g_key_to_index.end()) {
            results.push_back(it->second);
            found++;
        }
    }
    return found;
}

TM void update(int key) {
    auto it = g_key_to_index.find(key);
    if (it == g_key_to_index.end()) return;
    int idx = it->second;
    Record& rec = g_records[idx];
    for (int f = 0; f < NUM_FIELDS; f++) {
        for (int i = 0; i < FIELD_SIZE - 1; i++) {
            rec.data[f][i] = CHARS[(key + f + i + rec.timestamp) % 62];
        }
    }
    rec.timestamp = key;
}

TM int insert(int key) {
    if (g_record_count >= MAX_RECORDS) return -1;
    int idx = g_record_count;
    init_record(&g_records[idx], key);
    g_key_to_index[key] = idx;
    g_record_count++;
    g_record_counter++;
    return idx;
}

TX void txn_read(int key, char* out_data, int max_len) {
    read(key, out_data, max_len);
}

TX void txn_read_all(int key, char output[NUM_FIELDS][FIELD_SIZE]) {
    read_all_fields(key, output);
}

TX int txn_scan(int start_key, int count, std::vector<int>& results) {
    return scan(start_key, count, results);
}

TX void txn_update(int key) {
    update(key);
}

TX void txn_insert(int key) {
    insert(key);
}

TX void txn_read_modify_write(int key) {
    char data[FIELD_SIZE];
    read(key, data, FIELD_SIZE);
    update(key);
}

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
    std::atomic<uint64_t> ops{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> updates{0};
    std::atomic<uint64_t> inserts{0};
    std::atomic<uint64_t> scans{0};
    std::atomic<uint64_t> scan_results{0};
    int thread_id;
    WorkloadType workload;
    int key_range;
    std::mt19937* rng;
};

THREAD void worker(ThreadData* data) {
    std::uniform_int_distribution<int> uniform_dist(0, data->key_range - 1);
    std::uniform_real_distribution<double> real_dist(0.0, 1.0);

    data->barrier->wait();

    while (!stop_workers.load(std::memory_order_relaxed)) {
        int key;
        double r = real_dist(*data->rng);

        if (g_distribution == Distribution::ZIPFIAN) {
            key = zipfian_next(r * data->key_range / ZIPFIAN_ITEMS) % data->key_range;
        } else if (g_distribution == Distribution::LATEST) {
            int latest_bound = std::min(1000, data->key_range);
            key = (g_record_counter - (uniform_dist(*data->rng) % latest_bound) + data->key_range) % data->key_range;
        } else {
            key = uniform_dist(*data->rng);
        }

        int op_r = (int)(r * 100);

        switch (data->workload) {
        case WorkloadType::A:
            if (op_r < 50) {
                char buf[FIELD_SIZE];
                txn_read(key, buf, FIELD_SIZE);
                data->reads.fetch_add(1, std::memory_order_relaxed);
            } else {
                txn_update(key);
                data->updates.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case WorkloadType::B:
            if (op_r < 95) {
                char buf[FIELD_SIZE];
                txn_read(key, buf, FIELD_SIZE);
                data->reads.fetch_add(1, std::memory_order_relaxed);
            } else {
                txn_update(key);
                data->updates.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case WorkloadType::C: {
            char buf[FIELD_SIZE];
            txn_read(key, buf, FIELD_SIZE);
            data->reads.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case WorkloadType::D:
            if (op_r < 95) {
                char buf[FIELD_SIZE];
                txn_read(key, buf, FIELD_SIZE);
                data->reads.fetch_add(1, std::memory_order_relaxed);
            } else {
                int new_key = data->key_range + data->inserts.load();
                txn_insert(new_key);
                data->inserts.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case WorkloadType::E:
            if (op_r < 95) {
                char buf[FIELD_SIZE];
                txn_read(key, buf, FIELD_SIZE);
                data->reads.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::vector<int> results;
                txn_scan(key, 10, results);
                data->scans.fetch_add(1, std::memory_order_relaxed);
                data->scan_results.fetch_add(results.size(), std::memory_order_relaxed);
            }
            break;
        case WorkloadType::F:
            if (op_r < 50) {
                std::vector<int> results;
                txn_scan(key, 10, results);
                data->scans.fetch_add(1, std::memory_order_relaxed);
                data->scan_results.fetch_add(results.size(), std::memory_order_relaxed);
            } else {
                txn_read_modify_write(key);
                data->reads.fetch_add(1, std::memory_order_relaxed);
                data->updates.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        data->ops.fetch_add(1, std::memory_order_relaxed);
    }
}

MAIN int main(int argc, char* argv[]) {
    int nb_threads = DEFAULT_NB_THREADS;
    int duration_ms = DEFAULT_DURATION_MS;
    WorkloadType workload = WorkloadType::A;
    int key_range = 10000;
    int initial_records = 10000;

    init_zipfian(ZIPFIAN_ITEMS, ZIPFIAN_CONSTANT);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            nb_threads = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_ms = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            char w = argv[++i][0];
            switch (w) {
            case 'a': case 'A': workload = WorkloadType::A; break;
            case 'b': case 'B': workload = WorkloadType::B; break;
            case 'c': case 'C': workload = WorkloadType::C; break;
            case 'd': case 'D': workload = WorkloadType::D; break;
            case 'e': case 'E': workload = WorkloadType::E; break;
            case 'f': case 'F': workload = WorkloadType::F; break;
            }
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            key_range = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            initial_records = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-dist") == 0 && i + 1 < argc) {
            char d = argv[++i][0];
            if (d == 'u' || d == 'U') g_distribution = Distribution::UNIFORM;
            else if (d == 'z' || d == 'Z') g_distribution = Distribution::ZIPFIAN;
            else if (d == 'l' || d == 'L') g_distribution = Distribution::LATEST;
        }
    }

    const char* workload_names[6] = {"A", "B", "C", "D", "E", "F"};
    const char* dist_names[3] = {"uniform", "zipfian", "latest"};

    std::cout << "YCSB Benchmark (Full Specification)\n"
              << "==================================\n"
              << "Workload:    " << workload_names[(int)workload] << "\n"
              << "Distribution: " << dist_names[(int)g_distribution] << "\n"
              << "Threads:     " << nb_threads << "\n"
              << "Duration:    " << duration_ms << " ms\n"
              << "Key range:   " << key_range << "\n"
              << std::endl;

    std::cout << "Loading " << initial_records << " records..." << std::endl;
    for (int i = 0; i < initial_records; i++) {
        txn_insert(i);
    }
    std::cout << "  Loaded: " << g_record_count << " records\n"
              << std::endl;

    Barrier barrier(nb_threads);
    std::vector<ThreadData> thread_data(nb_threads);
    std::vector<std::thread> threads;
    std::vector<std::mt19937> rngs(nb_threads);

    for (int i = 0; i < nb_threads; i++) {
        rngs[i] = std::mt19937(i * 12345 + 42);
        thread_data[i].barrier = &barrier;
        thread_data[i].thread_id = i;
        thread_data[i].workload = workload;
        thread_data[i].key_range = key_range;
        thread_data[i].rng = &rngs[i];
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < nb_threads; i++) {
        threads.emplace_back(worker, &thread_data[i]);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    stop_workers.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    uint64_t total_ops = 0, total_reads = 0, total_updates = 0, total_inserts = 0, total_scans = 0, total_scan_results = 0;
    for (auto& td : thread_data) {
        total_ops += td.ops.load();
        total_reads += td.reads.load();
        total_updates += td.updates.load();
        total_inserts += td.inserts.load();
        total_scans += td.scans.load();
        total_scan_results += td.scan_results.load();
    }

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed time:  " << elapsed_ms << " ms\n"
              << "Total ops:     " << total_ops << "\n"
              << "Ops/sec:       " << (total_ops * 1000.0 / elapsed_ms) << "\n"
              << "  Reads:       " << total_reads << " (" << (total_ops > 0 ? total_reads * 100.0 / total_ops : 0) << "%)\n"
              << "  Updates:     " << total_updates << " (" << (total_ops > 0 ? total_updates * 100.0 / total_ops : 0) << "%)\n"
              << "  Inserts:     " << total_inserts << " (" << (total_ops > 0 ? total_inserts * 100.0 / total_ops : 0) << "%)\n"
              << "  Scans:       " << total_scans << " (" << (total_ops > 0 ? total_scans * 100.0 / total_ops : 0) << "%)\n"
              << "  Scan results: " << total_scan_results << "\n"
              << std::endl;

    std::cout << "Final record count: " << g_record_count << "\n";

    return 0;
}