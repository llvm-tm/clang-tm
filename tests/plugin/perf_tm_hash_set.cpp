// Performance benchmark: TMSafeHashSet vs std::unordered_set
// Compile: clang++ -std=c++20 -O3 -march=native -DNDEBUG -I../../backends/tm_impl/common perf_tm_hash_set.cpp -o perf_tm_hash_set

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <random>
#include <unordered_set>
#include <algorithm>

#include "../../backends/tm_impl/common/tm_hash_set.hpp"

using Clock = std::chrono::steady_clock;

struct Timer {
    Clock::time_point start;
    void   begin() { start = Clock::now(); }
    double elapsed() const {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
};

static std::vector<int> shuffled(size_t n, unsigned seed) {
    std::vector<int> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (int)i;
    std::shuffle(v.begin(), v.end(), std::mt19937(seed));
    return v;
}

static std::vector<int> random_keys(size_t n, unsigned seed, int range = 1 << 25) {
    std::vector<int> v(n);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, range - 1);
    for (size_t i = 0; i < n; i++) v[i] = dist(rng);
    return v;
}

static const int N = 500000;
static const int TRIALS = 5;

// Optimization sink — force materialization of loop results.
static size_t sink_value;
static inline void sink(size_t v) { sink_value = v; }

template <typename Set>
double bench_insert(Set& s, const std::vector<int>& keys) {
    Timer t; t.begin();
    for (int k : keys) s.insert(k);
    return t.elapsed();
}

template <typename Set>
double bench_contains_hit(Set& s, const std::vector<int>& keys) {
    size_t c = 0;
    Timer t; t.begin();
    for (int k : keys) if (s.contains(k)) c++;
    sink(c);
    return t.elapsed();
}

template <typename Set>
double bench_contains_miss(Set& s, const std::vector<int>& keys, int miss_base) {
    size_t c = 0;
    Timer t; t.begin();
    for (int k : keys) if (s.contains(k + miss_base)) c++;
    sink(c);
    return t.elapsed();
}

template <typename Set>
double bench_erase(Set& s, const std::vector<int>& keys) {
    Timer t; t.begin();
    for (int k : keys) s.erase(k);
    return t.elapsed();
}

template <typename Set>
double bench_iterate(Set& s) {
    size_t c = 0;
    Timer t; t.begin();
    for (auto& v : s) c += (size_t)v;
    sink(c);
    return t.elapsed();
}

template <typename Set>
double bench_mixed(Set& s, const std::vector<int>& keys, int miss_base) {
    size_t c = 0;
    Timer t; t.begin();
    size_t half = keys.size() / 2;
    for (size_t i = 0; i < half; i++) {
        s.insert(keys[i]);
        if (s.contains(keys[i])) c++;
        if (s.contains(keys[i] + miss_base)) c++;
    }
    for (size_t i = half; i < keys.size(); i++)
        s.erase(keys[i - half]);
    sink(c);
    return t.elapsed();
}

struct Result {
    double vals[TRIALS];
    void record(int trial, double v) { vals[trial] = v; }
    double median() const {
        double c[TRIALS];
        for (int i = 0; i < TRIALS; i++) c[i] = vals[i];
        std::sort(c, c + TRIALS);
        return c[TRIALS / 2];
    }
};

static void run_benchmark(const char* label, const std::vector<int>& keys,
                          const std::vector<int>& queries_hit,
                          const std::vector<int>& queries_miss) {
    printf("\n=== %s ===\n", label);

    Result ins_tm, ins_stl, hit_tm, hit_stl, miss_tm, miss_stl;
    Result era_tm, era_stl, iter_tm, iter_stl, mix_tm, mix_stl;

    for (int t = 0; t < TRIALS; t++) {
        {
            TMSafeHashSet<int> s;
            ins_tm.record(t, bench_insert(s, keys));
            hit_tm.record(t, bench_contains_hit(s, queries_hit));
            miss_tm.record(t, bench_contains_miss(s, queries_miss, 1 << 25));
            iter_tm.record(t, bench_iterate(s));
            era_tm.record(t, bench_erase(s, keys));
        }
        {
            std::unordered_set<int> s;
            s.reserve(keys.size() * 2);
            ins_stl.record(t, bench_insert(s, keys));
            hit_stl.record(t, bench_contains_hit(s, queries_hit));
            miss_stl.record(t, bench_contains_miss(s, queries_miss, 1 << 25));
            iter_stl.record(t, bench_iterate(s));
            era_stl.record(t, bench_erase(s, keys));
        }
        {
            TMSafeHashSet<int> s;
            mix_tm.record(t, bench_mixed(s, keys, 1 << 25));
        }
        {
            std::unordered_set<int> s;
            s.reserve(keys.size() * 2);
            mix_stl.record(t, bench_mixed(s, keys, 1 << 25));
        }
    }

    auto pr = [](const char* op, const Result& tm, const Result& stl) {
        double t = tm.median(), u = stl.median();
        printf("  %-12s  TM: %8.4f s   STL: %8.4f s   ratio: %.2f\n", op, t, u, t / u);
    };

    pr("insert",       ins_tm,  ins_stl);
    pr("contains(hit)", hit_tm, hit_stl);
    pr("contains(miss)",miss_tm,miss_stl);
    pr("erase",        era_tm,  era_stl);
    pr("iterate",      iter_tm, iter_stl);
    pr("mixed",        mix_tm,  mix_stl);
}

int main() {
    printf("TMSafeHashSet vs std::unordered_set  (%d x %d trials)\n", N, TRIALS);

    auto keys        = shuffled(N, 42);
    auto keys2       = random_keys(N, 99);
    auto queries_hit = random_keys(N, 1);
    for (auto& q : queries_hit) q = q % N;

    run_benchmark("shuffled keys", keys, queries_hit, keys2);

    std::vector<int> sorted_keys(N);
    for (int i = 0; i < N; i++) sorted_keys[i] = i;
    auto sq = shuffled(N, 77);
    for (auto& q : sq) q = q % N;
    run_benchmark("sorted keys", sorted_keys, sq, keys2);

    return 0;
}
