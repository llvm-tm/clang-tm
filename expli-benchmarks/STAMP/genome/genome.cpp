// STAMP/genome benchmark — explicit TM API port
// Matches the plugin genome_bench.hpp algorithm.
//
// Uses std:: containers with mutex for serialization, matching the
// plugin's tm_serialize_lock/unlock inside TX functions.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <random>

using PRNG = std::mt19937_64;

// ── Genome data ───────────────────────────────────────────
struct GenomeData {
    std::string gene;
    std::vector<std::string> segments;
    std::unordered_set<std::string> unique_segments;
    std::vector<std::string> reconstructed;
    int segment_length;
    int num_segments;
};

static GenomeData g_data;
static std::mutex g_mutex;              // matches tm_serialize_lock
static std::atomic<uint64_t> g_total_ops{0};

// ── TM-safe dedup (serialized via mutex) ──────────────────
static void genome_dedup(int start, int end) {
    for (int i = start; i < end; i++) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_data.unique_segments.insert(g_data.segments[i]);
    }
}

static inline uint64_t str_hash(const std::string& s, int start, int len) {
    uint64_t h = 0;
    for (int i = start; i < start + len; i++)
        h = h * 131 + (unsigned char)s[i];
    return h;
}

// ── Genome matching — uses mutex for shared reconstructed ─
// String operations (compare, substr, +) are not TM-protected,
// matching the plugin's tm_allow_opaque annotation.
static void genome_match(int start, int end,
                          std::unordered_map<uint64_t, std::vector<std::string*>>& hash_table) {
    // Build hash table from unique_segments
    for (auto it = g_data.unique_segments.begin(); it != g_data.unique_segments.end(); ++it) {
        uint64_t h = str_hash(*it, 1, (int)it->size() - 1);
        hash_table[h].push_back(const_cast<std::string*>(&(*it)));
    }

    // Find overlapping segments
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
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_data.reconstructed.push_back(*it + candidate->substr(j));
                        return;
                    }
                }
            }
        }
    }
}

// ── Worker thread ─────────────────────────────────────────
static void worker(int thread_id, int num_threads) {
    expli::TM<int>::thread_init();

    int nsegments = g_data.num_segments;
    int chunk = (nsegments + num_threads - 1) / num_threads;
    int start = thread_id * chunk;
    int end = std::min(start + chunk, nsegments);

    if (start < end)
        genome_dedup(start, end);

    std::unordered_map<uint64_t, std::vector<std::string*>> hash_table;

    if (start < end) {
        genome_match(start, end, hash_table);
        if (thread_id == 0)
            g_total_ops.fetch_add(g_data.unique_segments.size(), std::memory_order_relaxed);
    }

    expli::TM<int>::thread_exit();
}

int main(int argc, char* argv[]) {
    int num_threads = 4;
    int gene_length = 16384;
    int segment_length = 64;
    int num_segments = 16777216;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gene_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) segment_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) num_segments = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -g <gene> -s <segment> -n <segments>\n", argv[0]);
            return 0;
        }
    }

    printf("Creating gene and segments... done.\n");
    printf("Gene length     = %i\n", gene_length);
    printf("Segment length  = %i\n", segment_length);
    printf("Number segments = %i\n", num_segments);
    printf("Sequencing gene...\n");
    fflush(stdout);

    if (start < end) dedup(start, end);
}

    g_data.segment_length = segment_length;
    g_data.num_segments = num_segments;

    // Generate gene
    g_data.gene.resize(gene_length);
    PRNG rng(42);
    const char bases[] = {'a', 'c', 'g', 't'};
    for (int i = 0; i < gene_length; i++)
        g_data.gene[i] = bases[(int)(rng() % 4)];

    // Generate segments
    g_data.segments.resize(num_segments);
    for (int i = 0; i < num_segments; i++) {
        int start = (int)(rng() % (gene_length - segment_length));
        g_data.segments[i] = g_data.gene.substr(start, segment_length);
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++)
        threads.emplace_back(worker, i, num_threads);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    printf("    Time = %lld ms\n", (long long)elapsed);
    printf("    Unique segments = %zu\n", g_data.unique_segments.size());

    expli::TM<int>::exit();
    return 0;
}
