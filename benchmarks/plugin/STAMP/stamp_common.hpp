#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

extern "C" {
void tm_serialize_lock();
void tm_serialize_unlock();
int tm_serialize_unlock_all();
extern void* (*tm_calloc)(size_t, size_t);
}

// RAII helpers for TM-region allocation (zero-initialised).
// Use these in non-TX init code where the plugin cannot redirect new.
template <typename T> static T* tm_new()     { return (T*)tm_calloc(1, sizeof(T)); }
template <typename T> static T* tm_new_array(size_t n) { return (T*)tm_calloc(n, sizeof(T)); }

// TM-safe memory operations (no opaque libc calls)
static inline void tm_memcpy(char* dst, const char* src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

static inline int tm_strcmp(const char* a, const char* b, int alen, int blen) {
    int min = alen < blen ? alen : blen;
    for (int i = 0; i < min; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

static inline int tm_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("shared"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// TM-safe math — inline bodies visible to the TM instrumentation pass
// (libm sqrt/acos are opaque shared-library calls with no IR body)
static inline double tm_sqrt(double x) {
    if (x <= 0) return 0;
    double s = x;
    for (int i = 0; i < 25; i++) s = (s + x / s) * 0.5;
    return s;
}

static inline double tm_acos(double x) {
    if (x >= 1.0) return 0.0;
    if (x <= -1.0) return 3.14159265358979323846;
    // acos(x) = 2 * asin(sqrt((1-x)/2))
    // asin(z) ≈ z + z³/6 + 3z⁵/40 + 5z⁷/112 + 35z⁹/1152
    double z = tm_sqrt((1.0 - x) * 0.5);
    double z2 = z * z;
    double asin_z = z * (1.0 + z2 * (1.0/6.0 + z2 * (3.0/40.0 + z2 * (5.0/112.0 + z2 * 35.0/1152.0))));
    return 2.0 * asin_z;
}

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
extern double g_yada_jitter;  // -j: jitter magnitude for synthetic mesh
extern const char* g_yada_i;  // -i: input file prefix (.mesh file)

void run_benchmark(BenchmarkType bench, int threads);

__attribute__((annotate("tm_allow_opaque")))
inline uint64_t wall_clock_now() {
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
