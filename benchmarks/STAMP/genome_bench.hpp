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
    data->segment_length = 32;
    data->num_segments = 1 << 12;

    int gene_length = data->segment_length + data->num_segments + 100;
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
        data->unique_segments.insert(data->segments[i]);
    }
}

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
    }

    total_ops.fetch_add(data->unique_segments.size(), std::memory_order_relaxed);
}
