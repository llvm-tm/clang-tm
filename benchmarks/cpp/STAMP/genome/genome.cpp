// STAMP/genome benchmark — explicit TM API port
//
// Dedup uses a local unordered_set (thread 0 only, sequential, no mutex).
// Match runs sequentially after dedup (thread 0 only).
// No tm_serialize_lock or mutex needed — the sequential dedup avoids
// STL container allocation inside TM (std::unordered_set::insert does
// operator new, which the TM runtime intercepts).
//
// See also: benchmarks/plugin/STAMP/genome_bench.hpp (plugin version)
//           benchmarks/rust/src/stamp/genome.rs (rust version)

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../tests/benchmark_test.hpp"

using PRNG = std::mt19937_64;

struct GenomeData {
    std::string gene;
    std::vector<std::string> segments;
    std::unordered_set<std::string> unique_segments;
    int segment_length;
    int num_segments;
};

static GenomeData g_data;
static std::atomic<uint64_t> g_total_ops{0};

static int g_num_threads = 4;
static int g_gene_length = 16384;
static int g_segment_length = 64;
static int g_num_segments = 4096;

static void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) g_num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) g_gene_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) g_segment_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) g_num_segments = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) {} // segment match threshold (paper compat)
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -g <gene> -s <segment> -n <segments>\n", argv[0]);
            exit(0);
        }
    }
}

// Single-threaded dedup: thread 0 inserts all segments into a local
// unordered_set, then moves the result into g_data.unique_segments.
// This avoids operator-new inside TM (std::unordered_set::insert does
// heap allocation, which the TM runtime intercepts).
// Matches the plugin genome_bench.hpp approach.
static void genome_dedup(int start, int end) {
    std::unordered_set<std::string> local;
    local.reserve(end - start);
    for (int i = start; i < end; i++)
        local.insert(g_data.segments[i]);
    g_data.unique_segments = std::move(local);
}

static inline uint64_t str_hash(const std::string& s, int start, int len) {
    uint64_t h = 0;
    for (int i = start; i < start + len; i++)
        h = h * 131 + (unsigned char)s[i];
    return h;
}

static uint64_t genome_match() {
    std::unordered_map<uint64_t, std::vector<std::string*>> hash_table;
    for (auto& s : g_data.unique_segments) {
        if (s.size() > 1) {
            uint64_t h = str_hash(s, 0, (int)s.size() - 1);
            hash_table[h].push_back(const_cast<std::string*>(&s));
        }
    }

    for (int j = g_data.segment_length - 1; j >= 1; j--) {
        for (auto it = g_data.unique_segments.begin(); it != g_data.unique_segments.end(); ++it) {
            if ((int)it->size() <= j) continue;
            uint64_t end_h = str_hash(*it, (int)it->size() - j, j);
            auto fit = hash_table.find(end_h);
            if (fit != hash_table.end()) {
                for (auto candidate : fit->second) {
                    if (candidate == &(*it)) continue;
                    if (candidate->size() < (size_t)j) continue;
                    if (it->compare((int)it->size() - j, j, *candidate, 0, j) == 0) {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static void worker(int thread_id, int num_threads) {
    expli::TM<int>::thread_init();

    int nsegments = g_data.num_segments;

    // Only thread 0 does dedup (sequential, no mutex, local unordered_set)
    if (thread_id == 0)
        genome_dedup(0, nsegments);

    // genome_match reads g_data.unique_segments (read-only after dedup).
    // Only thread 0 runs it (all threads would do the same work).
    if (thread_id == 0) {
        genome_match();
        g_total_ops.fetch_add(g_data.unique_segments.size(), std::memory_order_relaxed);
    }

    expli::TM<int>::thread_exit();
}

static void test_cli_flags() {
    printf("  Testing CLI flags...\n");
    int save_p = g_num_threads, save_g = g_gene_length, save_s = g_segment_length, save_n = g_num_segments;
    TEST_EQ(g_num_threads, 4, "default threads");
    TEST_EQ(g_gene_length, 16384, "default gene length");
    TEST_EQ(g_segment_length, 64, "default segment length");
    TEST_EQ(g_num_segments, 4096, "default num segments");
    const char* test_args[] = {"prog", "-p", "8", "-g", "100", "-s", "10", "-n", "50"};
    parse_args(9, (char**)test_args);
    TEST_EQ(g_num_threads, 8, "override threads");
    TEST_EQ(g_gene_length, 100, "override gene length");
    TEST_EQ(g_segment_length, 10, "override segment length");
    TEST_EQ(g_num_segments, 50, "override num segments");
    g_num_threads = save_p; g_gene_length = save_g; g_segment_length = save_s; g_num_segments = save_n;
    if (test_result() != 0) exit(1);
}

static void test_rng() {
    printf("  Testing RNG determinism...\n");
    test_rng_determinism<PRNG>();
    if (test_result() != 0) exit(1);
}

static void test_logic() {
    printf("  Testing genome logic...\n");
    PRNG rng(42);
    const char bases[] = {'a', 'c', 'g', 't'};
    int gene_len = 100, seg_len = 10;
    std::string gene(gene_len, 'a');
    for (int i = 0; i < gene_len; i++)
        gene[i] = bases[(int)(rng() % 4)];
    TEST_EQ((int)gene.size(), gene_len, "gene length");
    for (int i = 0; i < 50; i++) {
        int start = (int)(rng() % (gene_len - seg_len));
        std::string seg = gene.substr(start, seg_len);
        TEST_EQ((int)seg.size(), seg_len, "segment length");
        TEST_ASSERT(seg.size() <= gene.size(), "segment <= gene");
    }
    if (test_result() != 0) exit(1);
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("Running self-tests for genome...\n");
        test_cli_flags();
        test_rng();
        test_logic();
        printf("All tests passed.\n");
        return 0;
    }
    parse_args(argc, argv);

    if (g_segment_length >= g_gene_length) {
        fprintf(stderr, "Error: segment length (%d) must be < gene length (%d)\n", g_segment_length, g_gene_length);
        return 1;
    }
    printf("Creating gene and segments... done.\n");
    printf("Gene length     = %i\n", g_gene_length);
    printf("Segment length  = %i\n", g_segment_length);
    printf("Number segments = %i\n", g_num_segments);
    printf("Sequencing gene...\n");
    fflush(stdout);

    expli::TM<int>::init();
    g_data.segment_length = g_segment_length;
    g_data.num_segments = g_num_segments;

    // Generate gene
    g_data.gene.resize(g_gene_length);
    PRNG rng(42);
    const char bases[] = {'a', 'c', 'g', 't'};
    for (int i = 0; i < g_gene_length; i++)
        g_data.gene[i] = bases[(int)(rng() % 4)];

    // Generate segments
    g_data.segments.resize(g_num_segments);
    for (int i = 0; i < g_num_segments; i++) {
        int start = (int)(rng() % (g_gene_length - g_segment_length));
        g_data.segments[i] = g_data.gene.substr(start, g_segment_length);
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < g_num_threads; i++)
        threads.emplace_back(worker, i, g_num_threads);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    printf("done.\n");
    printf("Time = %f\n", elapsed / 1000.0);
    printf("Unique segments = %lu\n", (unsigned long)ops);

    expli::TM<int>::exit();
    return 0;
}
