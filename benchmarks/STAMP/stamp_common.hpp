#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS = 4;

enum class BenchmarkType {
    BAYES,
    GENOME,
    INTRUDER,
    KMEANS,
    LABYRINTH,
    SSCA2,
    VACATION,
    YADA
};

struct Barrier {
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_ = 0;
    int num_threads_;

    explicit Barrier(int n) : num_threads_(n) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        count_++;
        if (count_ < num_threads_) {
            cv_.wait(lock);
        } else {
            count_ = 0;
            cv_.notify_all();
        }
    }
};

struct ThreadData {
    Barrier* barrier;
    int thread_id;
    int loops;
    BenchmarkType benchmark;
    void* data;
};

extern std::atomic<bool> stop_workers;
extern std::atomic<uint64_t> total_ops;
extern std::atomic<uint64_t> abort_count;

extern BenchmarkType g_benchmark;
extern int g_num_threads;
extern int g_duration;

void run_benchmark(BenchmarkType bench, int threads, int duration_ms);

inline uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

template <typename Engine>
class FastPRNG {
public:
    explicit FastPRNG(uint64_t seed) : engine_(seed) {}
    uint64_t next() { return engine_(); }
    uint64_t range(uint64_t max) {
        return engine_() % max;
    }
    double uniform() {
        return (double)engine_() / (double)engine_.max();
    }
    double uniform(double min, double max) {
        return min + (max - min) * uniform();
    }
private:
    Engine engine_;
};

using PRNG = FastPRNG<std::mt19937_64>;
