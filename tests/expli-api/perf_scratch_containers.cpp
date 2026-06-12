// Performance benchmark: ScratchVector vs std::vector, ScratchSet vs std::set/sorted vector
// Compile: clang++ -std=c++20 -O3 -march=native -DNDEBUG -I../../ -I../../backends/tm_impl/common perf_scratch_containers.cpp -o perf_scratch_containers

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <random>
#include <vector>
#include <set>
#include <algorithm>

#include "../../benchmarks/cpp/include/scratch_set.hpp"

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

static const int TRIALS = 7;

template <typename F>
static double measure(F&& f) {
    double best = 1e30;
    for (int t = 0; t < TRIALS; t++) {
        Timer tm; tm.begin();
        f();
        double e = tm.elapsed();
        if (e < best) best = e;
    }
    return best;
}

// =====================================================================
// ScratchVector vs std::vector
// =====================================================================
static void bench_vector(int N) {
    printf("\n=== ScratchVector vs std::vector (N=%d) ===\n", N);
    auto keys = shuffled(N, 42);

    // --- push_back (sequential) ---
    {
        double t = measure([&]() {
            ScratchVector<int> v;
            for (int i = 0; i < N; i++) v.push_back(i);
        });
        double u = measure([&]() {
            std::vector<int> v;
            v.reserve(N);
            for (int i = 0; i < N; i++) v.push_back(i);
        });
        printf("  push_back:      Scratch %8.4f s  std::vector %8.4f s  ratio %.2f\n", t, u, t / u);
    }

    // --- push_back (random order) ---
    {
        double t = measure([&]() {
            ScratchVector<int> v;
            for (int k : keys) v.push_back(k);
        });
        double u = measure([&]() {
            std::vector<int> v;
            v.reserve(N);
            for (int k : keys) v.push_back(k);
        });
        printf("  push_back(rnd): Scratch %8.4f s  std::vector %8.4f s  ratio %.2f\n", t, u, t / u);
    }

    // --- iterate (range-for) ---
    {
        ScratchVector<int> sv; for (int i = 0; i < N; i++) sv.push_back(i);
        std::vector<int>   stv; stv.reserve(N); for (int i = 0; i < N; i++) stv.push_back(i);
        double t = measure([&]() {
            volatile size_t sink = 0; for (auto x : sv) sink += (size_t)x; (void)sink;
        });
        double u = measure([&]() {
            volatile size_t sink = 0; for (auto x : stv) sink += (size_t)x; (void)sink;
        });
        printf("  iterate:        Scratch %8.4f s  std::vector %8.4f s  ratio %.2f\n", t, u, t / u);
    }

    // --- random access ---
    {
        ScratchVector<int> sv; for (int i = 0; i < N; i++) sv.push_back(i);
        std::vector<int>   stv; stv.reserve(N); for (int i = 0; i < N; i++) stv.push_back(i);
        auto idx = shuffled(N, 7);
        double t = measure([&]() {
            volatile size_t sink = 0; for (int i : idx) sink += (size_t)sv[i]; (void)sink;
        });
        double u = measure([&]() {
            volatile size_t sink = 0; for (int i : idx) sink += (size_t)stv[i]; (void)sink;
        });
        printf("  random access:  Scratch %8.4f s  std::vector %8.4f s  ratio %.2f\n", t, u, t / u);
    }
}

// =====================================================================
// ScratchSet vs std::set vs sorted std::vector
// =====================================================================
static void bench_set(int N) {
    printf("\n=== ScratchSet vs std::set vs sorted std::vector (N=%d) ===\n", N);
    auto keys  = shuffled(N, 42);
    auto hits  = shuffled(N, 1);
    auto miss  = shuffled(N, 99);
    for (auto& x : miss) x += N;  // ensure all misses

    // === INSERT ===
    double ins_ss = measure([&]() {
        ScratchSet<int> s; for (int k : keys) s.insert(k);
    });
    double ins_stl = measure([&]() {
        std::set<int> s; for (int k : keys) s.insert(k);
    });
    double ins_sv = measure([&]() {
        std::vector<int> v; v.reserve(N); for (int k : keys) v.push_back(k);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    });
    printf("  insert:         ScratchSet %8.4f s  std::set %8.4f s  sorted_vec %8.4f s\n",
           ins_ss, ins_stl, ins_sv);

    // === CONTAINS (hit) ===
    {
        ScratchSet<int> ss; for (int k : keys) ss.insert(k);
        std::set<int> stl;  for (int k : keys) stl.insert(k);
        std::vector<int> sv; sv.reserve(N);
        for (int k : keys) sv.push_back(k);
        std::sort(sv.begin(), sv.end());
        sv.erase(std::unique(sv.begin(), sv.end()), sv.end());

        double t = measure([&]() {
            volatile size_t sink = 0; for (int k : hits) if (ss.contains(k)) sink++; (void)sink;
        });
        double u = measure([&]() {
            volatile size_t sink = 0; for (int k : hits) if (stl.contains(k)) sink++; (void)sink;
        });
        double v = measure([&]() {
            volatile size_t sink = 0;
            for (int k : hits) if (std::binary_search(sv.begin(), sv.end(), k)) sink++;
            (void)sink;
        });
        printf("  contains(hit):  ScratchSet %8.4f s  std::set %8.4f s  sorted_vec %8.4f s\n", t, u, v);
    }

    // === CONTAINS (miss) ===
    {
        ScratchSet<int> ss; for (int k : keys) ss.insert(k);
        std::set<int> stl;  for (int k : keys) stl.insert(k);
        std::vector<int> sv; sv.reserve(N);
        for (int k : keys) sv.push_back(k);
        std::sort(sv.begin(), sv.end());
        sv.erase(std::unique(sv.begin(), sv.end()), sv.end());

        double t = measure([&]() {
            volatile size_t sink = 0; for (int k : miss) if (ss.contains(k)) sink++; (void)sink;
        });
        double u = measure([&]() {
            volatile size_t sink = 0; for (int k : miss) if (stl.contains(k)) sink++; (void)sink;
        });
        double v = measure([&]() {
            volatile size_t sink = 0;
            for (int k : miss) if (std::binary_search(sv.begin(), sv.end(), k)) sink++;
            (void)sink;
        });
        printf("  contains(miss): ScratchSet %8.4f s  std::set %8.4f s  sorted_vec %8.4f s\n", t, u, v);
    }

    // === ERASE ===
    {
        ScratchSet<int> ss; for (int k : keys) ss.insert(k);
        std::set<int> stl;  for (int k : keys) stl.insert(k);
        // Sorted vec erase is O(n), only benchmark for small N

        double t = measure([&]() {
            for (int k : keys) ss.erase(k);
        });
        double u = measure([&]() {
            for (int k : keys) stl.erase(k);
        });
        printf("  erase:          ScratchSet %8.4f s  std::set %8.4f s  (vec skip, O(n))\n", t, u);
    }

    // === ITERATE ===
    {
        ScratchSet<int> ss; for (int k : keys) ss.insert(k);
        std::set<int> stl;  for (int k : keys) stl.insert(k);
        std::vector<int> sv(ss.begin(), ss.end());

        double t = measure([&]() {
            volatile size_t sink = 0; for (auto x : ss) sink += (size_t)x; (void)sink;
        });
        double u = measure([&]() {
            volatile size_t sink = 0; for (auto x : stl) sink += (size_t)x; (void)sink;
        });
        double v = measure([&]() {
            volatile size_t sink = 0; for (auto x : sv) sink += (size_t)x; (void)sink;
        });
        printf("  iterate:        ScratchSet %8.4f s  std::set %8.4f s  sorted_vec %8.4f s\n", t, u, v);
    }
}

int main() {
    printf("Scratch containers vs STL — performance comparison\n");
    printf("(best of %d trials, -O3 -march=native)\n\n", TRIALS);

    bench_vector(100000);
    bench_vector(1000000);

    bench_set(10000);
    bench_set(100000);

    return 0;
}
