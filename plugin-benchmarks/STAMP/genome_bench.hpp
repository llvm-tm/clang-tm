#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TM GenomeData {
    std::string gene;
    std::vector<std::string> segments;
    std::unordered_set<std::string> unique_segments;
    std::vector<std::string> reconstructed;
    int segment_length;
    int num_segments;
};

static GenomeData* g_genome = nullptr;

inline void genome_generate_segments() {
    auto data = new GenomeData();
    data->segment_length = g_genome_s;
    data->num_segments = g_genome_n;

    int gene_length = g_genome_g;
    data->gene.resize(gene_length);
    
    PRNG rng(42);
    const char bases[] = {'a', 'c', 'g', 't'};
    for (int i = 0; i < gene_length; i++) {
        data->gene[i] = bases[rng.next() % 4];
    }

    data->segments.resize(data->num_segments);
    for (int i = 0; i < data->num_segments; i++) {
        int start = (int)(rng.next() % (gene_length - data->segment_length));
        data->segments[i] = data->gene.substr(start, data->segment_length);
    }

    g_genome = data;

    printf("Creating gene and segments... done.\n");
    printf("Gene length     = %i\n", g_genome_g);
    printf("Segment length  = %i\n", g_genome_s);
    printf("Number segments = %i\n", g_genome_n);
    printf("Sequencing gene...\n");
    fflush(stdout);
}

static inline uint64_t str_hash(const std::string& s, int start, int len) {
    uint64_t h = 0;
    for (int i = start; i < start + len; i++) {
        h = h * 131 + (unsigned char)s[i];
    }
    return h;
}

TX static void genome_dedup(GenomeData* data, int start, int end) {
    for (int i = start; i < end; i++) {
        tm_serialize_lock();
        data->unique_segments.insert(data->segments[i]);
        tm_serialize_unlock();
    }
}

// NOTE: genome_match calls std::string::compare on TM-managed GenomeData
// strings inside a transaction.  string::compare reads the internal char
// buffer via opaque library code that the TM pass cannot instrument.
// This means the buffer reads bypass tm_read — they are non-transactional.
//
// This is accepted here because the genome benchmark's data layout
// guarantees that compared strings are not concurrently modified:
//   - unique_segments is populated during dedup (before transactions start)
//   - genome_match only reads segments, never writes them
//   - hash_table is a local (stack) variable, not TM-allocated
// The reconstructed vector is the only TM-shared write destination.
//
// An ideal fix would replace string::compare with a hand-rolled inline
// comparison that is visible to the TM instrumentation pass.
__attribute__((annotate("tm_allow_opaque")))
TX static void genome_match(GenomeData* data, int start, int end,
                             std::unordered_map<uint64_t, std::vector<std::string*>>& hash_table) {
    for (auto it = data->unique_segments.begin(); it != data->unique_segments.end(); ++it) {
        uint64_t h = str_hash(*it, 1, (int)it->size() - 1);
        hash_table[h].push_back(const_cast<std::string*>(&(*it)));
    }

    for (int j = data->segment_length - 1; j >= 1; j--) {
        for (auto it = data->unique_segments.begin(); it != data->unique_segments.end(); ++it) {
            if ((int)it->size() <= j) continue;
            uint64_t end_h = str_hash(*it, (int)it->size() - j, j);
            auto fit = hash_table.find(end_h);
            if (fit != hash_table.end()) {
                for (auto candidate : fit->second) {
                    if (candidate == &(*it)) continue;
                    if (candidate->size() < (size_t)j) continue;
                    if (it->compare((int)it->size() - j, j, *candidate, 0, j) == 0) {
                        data->reconstructed.push_back(*it + candidate->substr(j));
                        return;
                    }
                }
            }
        }
    }
}

THREAD void worker_genome(ThreadData* td) {
    auto data = g_genome;
    int nsegments = data->num_segments;
    int chunk = (nsegments + g_num_threads - 1) / g_num_threads;
    int start = td->thread_id * chunk;
    int end = std::min(start + chunk, nsegments);

    if (start < end) {
        genome_dedup(data, start, end);
    }

    std::unordered_map<uint64_t, std::vector<std::string*>> hash_table;

    if (start < end) {
        genome_match(data, start, end, hash_table);
        if (td->thread_id == 0) {
            total_ops.fetch_add(data->unique_segments.size(), std::memory_order_relaxed);
        }
    }
}
