#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

extern "C" {
void tm_serialize_lock();
void tm_serialize_unlock();
}

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

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

extern std::atomic<uint64_t> total_ops;
extern std::atomic<uint64_t> abort_count;

extern BenchmarkType g_benchmark;
extern int g_num_threads;

// Benchmark-specific command-line parameters
extern int g_bayes_v;         // -v: number of variables
extern int g_bayes_r;         // -r: number of records
extern int g_bayes_n;         // -n: max parents
extern int g_bayes_p;         // -p: percent chance of parent
extern int g_bayes_s;         // -s: random seed
extern int g_bayes_i;         // -i: edge insert penalty
extern int g_bayes_e;         // -e: max edges learned per variable

extern int g_genome_g;        // -g: gene length
extern int g_genome_s;        // -s: segment length
extern int g_genome_n;        // -n: number of segments

extern int g_intruder_a;      // -a: percent of attacks
extern int g_intruder_l;      // -l: max packets per stream
extern int g_intruder_n;      // -n: total number of streams
extern int g_intruder_s;      // -s: random seed

extern int g_kmeans_m;        // -m: max clusters
extern int g_kmeans_n;        // -n: min clusters
extern double g_kmeans_t;     // -t: threshold
extern const char* g_kmeans_i;// -i: input file

extern int g_labyrinth_x;     // -x: grid x dimension
extern int g_labyrinth_y;     // -y: grid y dimension
extern int g_labyrinth_z;     // -z: grid z dimension
extern int g_labyrinth_n;     // -n: number of paths

extern int g_ssca2_s;         // -s: problem scale
extern int g_ssca2_i;         // -i: iterations
extern double g_ssca2_u;      // -u: probability unidirectional
extern int g_ssca2_l;         // -l: max path length
extern int g_ssca2_p;         // -p: max parallel edges

extern int g_vacation_n;      // -n: queries per task
extern int g_vacation_q;      // -q: percent of relations queried
extern int g_vacation_r;      // -r: number of possible relations
extern int g_vacation_u;      // -u: percent of user tasks
extern int g_vacation_t;      // -t: total number of tasks

extern int g_yada_angle;      // -a: angle constraint
extern const char* g_yada_i;  // -i: input file prefix

void run_benchmark(BenchmarkType bench, int threads);

inline uint64_t rdtsc() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
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
