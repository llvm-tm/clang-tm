// Genome — C++ port of the original STAMP spec (explicit API path)
// Original spec: https://github.com/ccaominh/stamp/tree/master/genome
//
// Parameters:
//   -g <num>   Gene length       (default: 16384)
//   -s <num>   Segment length    (default: 64)
//   -n <num>   Number of segments(default: 4096)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <random>

static long g_gene_length    = 16384;
static long g_segment_length = 64;
static long g_num_segments   = 4096;
static long g_num_threads    = 4;

// ── TM init (minimal — genome barely uses TM) ─────────────────────
extern "C" {
    void     tm_init();
    void     tm_exit();
    void     tm_init_thread();
    void     tm_exit_thread();
}

// ── PRNG (std::mt19937_64 matching plugin) ─────────────────────────
static thread_local std::mt19937_64 tls_rng;
static void rng_seed(uint64_t s) { tls_rng = std::mt19937_64(s); }
static uint64_t rng_next() { return tls_rng(); }

// ── Data ───────────────────────────────────────────────────────────
static std::string g_gene;
static std::vector<std::string> g_segments;
static std::unordered_set<std::string> g_unique_segments;
static std::vector<std::string> g_reconstructed;
static std::mutex g_dedup_mutex;
static std::atomic<long> g_total_ops{0};

// ── Hash function (matching plugin) ────────────────────────────────
static uint64_t str_hash(const std::string& s, size_t start, size_t len) {
    uint64_t h = 0;
    for (size_t i = start; i < start + len; i++)
        h = h * 131 + (unsigned char)s[i];
    return h;
}

// ── Dedup (mutex-protected, matching plugin's tm_serialize_lock) ───
static void dedup(long start, long end) {
    for (long i = start; i < end; i++) {
        std::lock_guard<std::mutex> lock(g_dedup_mutex);
        g_unique_segments.insert(g_segments[(size_t)i]);
    }
}

// ── Match (no TM needed — reads unique_set, writes reconstructed) ──
static void match() {
    // Build hash table of suffix hashes (skip first char)
    std::unordered_map<uint64_t, std::vector<std::string*>> hash_table;
    for (auto& s : g_unique_segments) {
        if (s.size() > 1) {
            uint64_t h = str_hash(s, 1, s.size() - 1);
            hash_table[h].push_back(const_cast<std::string*>(&s));
        }
    }

    // Find longest suffix-prefix overlap
    for (long j = g_segment_length - 1; j >= 1; j--) {
        for (auto& s : g_unique_segments) {
            if ((long)s.size() <= j) continue;
            uint64_t end_h = str_hash(s, s.size() - (size_t)j, (size_t)j);
            auto fit = hash_table.find(end_h);
            if (fit == hash_table.end()) continue;
            for (auto candidate : fit->second) {
                if (candidate == &s) continue;
                if ((long)candidate->size() < j) continue;
                if (s.compare(s.size() - (size_t)j, (size_t)j,
                              *candidate, 0, (size_t)j) == 0) {
                    g_reconstructed.push_back(s + candidate->substr((size_t)j));
                    return;
                }
            }
        }
    }
}

// ── Worker ─────────────────────────────────────────────────────────
static void worker(long tid) {
    tm_init_thread();

    long chunk = (g_num_segments + g_num_threads - 1) / g_num_threads;
    long start = tid * chunk;
    long end = std::min(start + chunk, g_num_segments);

    if (start < end) dedup(start, end);
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-g") == 0 && i+1 < argc)
            g_gene_length = atol(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc)
            g_segment_length = atol(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc)
            g_num_segments = atol(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc)
            g_num_threads = atol(argv[++i]);
    }

    printf("Genome (STAMP spec)\n");
    printf("  Gene length:    %ld\n", g_gene_length);
    printf("  Segment length: %ld\n", g_segment_length);
    printf("  Segments:       %ld\n", g_num_segments);
    printf("  Threads:        %ld\n", g_num_threads);

    tm_init();

    // Generate gene
    rng_seed(42);
    const char bases[] = {'a', 'c', 'g', 't'};
    g_gene.resize((size_t)g_gene_length);
    for (long i = 0; i < g_gene_length; i++)
        g_gene[(size_t)i] = bases[rng_next() % 4];

    // Generate segments (random substrings)
    g_segments.resize((size_t)g_num_segments);
    for (long i = 0; i < g_num_segments; i++) {
        long start = (long)(rng_next() % (uint64_t)(g_gene_length - g_segment_length));
        g_segments[(size_t)i] = g_gene.substr((size_t)start, (size_t)g_segment_length);
    }

    printf("  Generated %ld segments\n", g_num_segments);

    // Dedup (parallel, mutex-protected)
    auto t1 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (long t = 0; t < g_num_threads; t++)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    // Match (single-threaded, matching plugin's all-threads-but-only-one-counts)
    match();

    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    printf("\nResults (%ld ms):\n", (long)(elapsed * 1000));
    printf("  Unique segments: %zu\n", g_unique_segments.size());
    printf("  Reconstructed:   %zu\n", g_reconstructed.size());
    printf("  PASS\n");

    tm_exit();
    return 0;
}
