#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct TM GenomeData {
    char* gene;
    int gene_length;
    char** segments;
    int* segment_lens;
    int num_segments;
    char** unique_segments;
    int* unique_segment_lens;
    int unique_count;
    int unique_capacity;
    char** reconstructed;
    int reconstructed_count;
    int reconstructed_capacity;
    int segment_length;
};

static GenomeData* g_genome = nullptr;

static inline int seg_cmp(const char* a, int alen, const char* b, int blen) {
    int min_len = std::min(alen, blen);
    int r = std::strncmp(a, b, min_len);
    if (r != 0) return r;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

static int seg_find_insert_pos(char** strs, int* lens, int count,
                                const char* s, int slen) {
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = seg_cmp(strs[mid], lens[mid], s, slen);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

inline void genome_generate_segments() {
    int gene_len = g_genome_g;
    int seg_len = g_genome_s;
    int num_segs = g_genome_n;

    auto data = new GenomeData();
    data->gene_length = gene_len;
    data->segment_length = seg_len;
    data->num_segments = num_segs;

    data->gene = new char[gene_len + 1];

    PRNG rng(42);
    const char bases[] = {'a', 'c', 'g', 't'};
    for (int i = 0; i < gene_len; i++) {
        data->gene[i] = bases[rng.next() % 4];
    }
    data->gene[gene_len] = '\0';

    data->segments = new char*[num_segs]();
    data->segment_lens = new int[num_segs]();
    for (int i = 0; i < num_segs; i++) {
        int start = (int)(rng.next() % (gene_len - seg_len));
        data->segments[i] = new char[seg_len + 1];
        std::strncpy(data->segments[i], data->gene + start, seg_len);
        data->segments[i][seg_len] = '\0';
        data->segment_lens[i] = seg_len;
    }

    data->unique_segments = new char*[num_segs]();
    data->unique_segment_lens = new int[num_segs]();
    data->unique_count = 0;
    data->unique_capacity = num_segs;

    data->reconstructed = new char*[num_segs]();
    data->reconstructed_count = 0;
    data->reconstructed_capacity = num_segs;

    g_genome = data;

    printf("Creating gene and segments... done.\n");
    printf("Gene length     = %i\n", g_genome_g);
    printf("Segment length  = %i\n", g_genome_s);
    printf("Number segments = %i\n", g_genome_n);
    printf("Sequencing gene...\n");
    fflush(stdout);
}

static inline uint64_t str_hash(const char* s, int start, int len) {
    uint64_t h = 0;
    for (int i = start; i < start + len; i++) {
        h = h * 131 + (unsigned char)s[i];
    }
    return h;
}

// Sequential dedup: single-threaded (non-TX), uses local hash table.
// No serialize_lock needed — runs before parallel TM phase.
static void genome_dedup_seq(GenomeData* data) {
    std::unordered_map<uint64_t, int> dup_map;
    for (int i = 0; i < data->num_segments; i++) {
        char* seg = data->segments[i];
        int slen = data->segment_lens[i];
        uint64_t h = str_hash(seg, 0, slen);
        if (dup_map.find(h) != dup_map.end()) continue;
        dup_map[h] = i;
        data->unique_segments[data->unique_count] = seg;
        data->unique_segment_lens[data->unique_count] = slen;
        data->unique_count++;
    }
}

static void build_hash_table(GenomeData* data,
    std::unordered_map<uint64_t, std::vector<char**>>& hash_table) {
    for (int i = 0; i < data->unique_count; i++) {
        char* seg = data->unique_segments[i];
        int slen = data->unique_segment_lens[i];
        uint64_t h = str_hash(seg, 0, slen - 1);
        hash_table[h].push_back(&data->unique_segments[i]);
    }
}

__attribute__((annotate("tm_allow_opaque")))
TX static void genome_match(GenomeData* data, int start, int end,
                             std::unordered_map<uint64_t, std::vector<char**>>& hash_table) {
    for (int j = data->segment_length - 1; j >= 1; j--) {
        for (int i = 0; i < data->unique_count; i++) {
            char* a = data->unique_segments[i];
            int alen = data->unique_segment_lens[i];
            if (alen <= j) continue;
            uint64_t end_h = str_hash(a, alen - j, j);
            auto fit = hash_table.find(end_h);
            if (fit != hash_table.end()) {
                for (char** pcandidate : fit->second) {
                    if (pcandidate == &data->unique_segments[i]) continue;
                    char* b = *pcandidate;
                    int blen = data->unique_segment_lens[pcandidate - data->unique_segments];
                    if (blen < j) continue;
                    if (std::strncmp(a + alen - j, b, j) == 0) {
                        int new_len = alen + blen - j;
                        char* combined = new char[new_len + 1];
                        std::strcpy(combined, a);
                        std::strncat(combined, b + j, blen - j);
                        data->reconstructed[data->reconstructed_count++] = combined;
                        return;
                    }
                }
            }
        }
    }
}

static std::atomic<int> g_genome_barrier{0};

THREAD void worker_genome(ThreadData* td) {
    auto data = g_genome;

    // Thread 0 runs sequential dedup (non-TX, no serialize_lock)
    if (td->thread_id == 0) {
        genome_dedup_seq(data);
    }

    // Barrier: all threads wait for dedup to complete
    g_genome_barrier.fetch_add(1, std::memory_order_release);
    while (g_genome_barrier.load(std::memory_order_acquire) < g_num_threads) {}

    // Build per-thread hash table from unique segments
    std::unordered_map<uint64_t, std::vector<char**>> hash_table;
    build_hash_table(data, hash_table);

    // Parallel matching: each thread processes a portion of unique segments
    int chunk = (data->unique_count + g_num_threads - 1) / g_num_threads;
    int start = td->thread_id * chunk;
    int end = std::min(start + chunk, data->unique_count);

    if (start < end) {
        genome_match(data, start, end, hash_table);
        if (td->thread_id == 0) {
            total_ops.fetch_add((uint64_t)data->unique_count, std::memory_order_relaxed);
        }
    }
}
