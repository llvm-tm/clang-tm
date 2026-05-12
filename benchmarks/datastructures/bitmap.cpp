/**
 * Bitmap Benchmark - Modern C++17 Version
 *
 * Tests transactional memory with concurrent bitmap operations.
 * Uses a TM-annotated bit array for transactional access.
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
#include <cstdint>
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

constexpr int MAX_BITS = DEFAULT_RANGE_MAX;
constexpr int BITS_PER_WORD = 64;
constexpr int NUM_WORDS = (MAX_BITS + BITS_PER_WORD - 1) / BITS_PER_WORD;

// Bitmap using TM-annotated arrays
TM uint64_t bitmap[NUM_WORDS] = {0};
TM int bit_count = 0;

TM inline int wordIndex(int bit) { return bit / BITS_PER_WORD; }
TM inline int bitOffset(int bit) { return bit % BITS_PER_WORD; }

TM void setBit(int bit) {
    if (bit < 0 || bit >= MAX_BITS) return;
    int wi = wordIndex(bit);
    int bo = bitOffset(bit);
    uint64_t mask = 1ULL << bo;
    if ((bitmap[wi] & mask) == 0) {
        bit_count++;
    }
    bitmap[wi] |= mask;
}

TM void clearBit(int bit) {
    if (bit < 0 || bit >= MAX_BITS) return;
    int wi = wordIndex(bit);
    int bo = bitOffset(bit);
    uint64_t mask = 1ULL << bo;
    if ((bitmap[wi] & mask) != 0) {
        bit_count--;
    }
    bitmap[wi] &= ~mask;
}

TM bool isSet(int bit) {
    if (bit < 0 || bit >= MAX_BITS) return false;
    int wi = wordIndex(bit);
    int bo = bitOffset(bit);
    uint64_t mask = 1ULL << bo;
    return (bitmap[wi] & mask) != 0;
}

TM int count() {
    return bit_count;
}

TX void txn_setBit(int bit) {
    setBit(bit);
}

TX void txn_clearBit(int bit) {
    clearBit(bit);
}

TX bool txn_isSet(int bit) {
    return isSet(bit);
}


std::atomic<bool> stop_workers(false);

struct ThreadData {
    Barrier* barrier;
    std::atomic<uint64_t> nb_set{0};
    std::atomic<uint64_t> nb_clear{0};
    std::atomic<uint64_t> nb_test{0};
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
            txn_setBit(key);
            data->nb_set.fetch_add(1, std::memory_order_relaxed);
        } else if (op < data->write_pct + data->read_pct) {
            int key = key_dist(rng);
            bool set = txn_isSet(key);
            (void)set;
            data->nb_test.fetch_add(1, std::memory_order_relaxed);
        } else {
            int key = key_dist(rng);
            txn_clearBit(key);
            data->nb_clear.fetch_add(1, std::memory_order_relaxed);
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

    std::cout << "Bitmap Benchmark\n"
              << "==================\n"
              << "Threads:         " << nb_threads << "\n"
              << "Initial size:   " << initial_size << "\n"
              << "Duration:      " << duration_ms << " ms\n"
              << "Read %:        " << read_pct << "%\n"
              << "Write %:       " << write_pct << "%\n"
              << "Key range:      " << range_max << "\n"
              << std::endl;

    // Initialize bitmap with random bits
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, range_max - 1);
    for (int i = 0; i < initial_size; i++) {
        txn_setBit(key_dist(rng));
    }

    std::cout << "Initial bit count: " << count() << std::endl;

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

    uint64_t total_sets = 0, total_clears = 0, total_tests = 0;
    for (const auto& td : thread_data) {
        total_sets += td.nb_set.load();
        total_clears += td.nb_clear.load();
        total_tests += td.nb_test.load();
    }

    uint64_t total_ops = total_sets + total_clears + total_tests;
    int final_count = count();

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed time:  " << elapsed_ms << " ms\n"
              << "Final count:   " << final_count << "\n"
              << "Total sets:    " << total_sets << "\n"
              << "Total clears:   " << total_clears << "\n"
              << "Total tests:   " << total_tests << "\n"
              << "Total ops:     " << total_ops << "\n"
              << "Ops/sec:       " << (total_ops * 1000.0 / elapsed_ms) << "\n"
              << std::endl;

    return 0;
}