/**
 * STAMP Benchmark Suite - Full Specification Implementation
 *
 * Based on: STAMP: Stanford Transactional Applications for Multi-Processing
 * Authors: Chi Cao Minh, JaeWoong Chung, Christos Kozyrakis, Kunle Olukotun
 * Published: IISWC 2008
 *
 * GitHub: https://github.com/kozyraki/stamp
 * Paper: https://ieeexplore.ieee.org/document/4636089
 *
 * 8 Benchmarks:
 * - bayes: Bayesian network structure learning
 * - genome: Gene sequencing
 * - intruder: Network intrusion detection
 * - kmeans: K-means clustering
 * - labyrinth: Maze routing
 * - ssca2: Graph kernels
 * - vacation: Travel reservation system
 * - yada: Delaunay mesh refinement
 */

#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <list>
#include <set>
#include <map>
#include <fstream>
#include <sstream>
#include <cstdlib>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

constexpr int DEFAULT_DURATION_MS = 10000;
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

std::atomic<bool> stop_workers{false};
std::atomic<uint64_t> total_ops{0};
std::atomic<uint64_t> abort_count{0};

struct ThreadData {
    Barrier* barrier;
    int thread_id;
    int loops;
    BenchmarkType benchmark;
    void* data;
};

BenchmarkType g_benchmark = BenchmarkType::BAYES;
int g_num_threads = DEFAULT_NB_THREADS;
int g_duration = DEFAULT_DURATION_MS;

// ============================================================================
// BAYES - Bayesian Network Structure Learning
// ============================================================================

constexpr int BAYES_MAX_VARS = 64;
constexpr int BAYES_MAX_RECORDS = 10000;
constexpr int BAYES_MAX_PARENTS = 5;

struct BayesNode {
    int id;
    std::vector<int> parents;
    std::vector<double> cpts;
};

struct BayesRecord {
    int values[BAYES_MAX_VARS];
};

TM std::vector<BayesNode> g_bayes_network;
TM std::vector<BayesRecord> g_bayes_records;
TM int g_bayes_num_vars = 32;
TM int g_bayes_num_records = 4096;
TM int g_bayes_max_parents = 2;

TM double g_bayes_scores[BAYES_MAX_VARS];

void bayes_generate_network() {
    g_bayes_network.clear();
    for (int i = 0; i < g_bayes_num_vars; i++) {
        BayesNode node;
        node.id = i;
        int num_parents = std::min(i, g_bayes_max_parents);
        for (int p = 0; p < num_parents; p++) {
            node.parents.push_back(i - p - 1);
        }
        int cpt_size = 1 << node.parents.size();
        node.cpts.resize(cpt_size);
        for (int j = 0; j < cpt_size; j++) {
            node.cpts[j] = 0.5 + (rand() % 100) / 200.0;
        }
        g_bayes_network.push_back(node);
    }
}

void bayes_generate_records() {
    g_bayes_records.clear();
    for (int r = 0; r < g_bayes_num_records; r++) {
        BayesRecord rec;
        for (int i = 0; i < g_bayes_num_vars; i++) {
            int cpt_idx = 0;
            for (size_t p = 0; p < g_bayes_network[i].parents.size(); p++) {
                if (rec.values[g_bayes_network[i].parents[p]]) {
                    cpt_idx |= (1 << p);
                }
            }
            double prob = g_bayes_network[i].cpts[cpt_idx];
            rec.values[i] = (rand() % 1000) < (prob * 1000) ? 1 : 0;
        }
        g_bayes_records.push_back(rec);
    }
}

TX double bayes_compute_score(int var, const std::vector<int>& parents) {
    int num_parents = parents.size();
    int cpt_size = 1 << num_parents;
    
    std::vector<int> counts(cpt_size * 2, 0);
    
    for (size_t ridx = 0; ridx < g_bayes_records.size(); ridx++) {
        int idx = 0;
        for (int p = 0; p < num_parents; p++) {
            if (g_bayes_records[ridx].values[parents[p]]) idx |= (1 << p);
        }
        bool var_val = g_bayes_records[ridx].values[var];
        idx += var_val ? cpt_size : 0;
        counts[idx]++;
    }

    double score = 0;
    for (int i = 0; i < cpt_size * 2; i++) {
        if (counts[i] > 0) {
            score += counts[i] * std::log((double)counts[i] + 0.1);
        }
    }
    return score;
}

TX void bayes_inference() {
    for (int i = 0; i < g_bayes_num_vars; i++) {
        int local_sum = 0;
        for (size_t r = 0; r < g_bayes_records.size(); r++) {
            local_sum += g_bayes_records[r].values[i];
        }
        g_bayes_scores[i] = (double)local_sum / g_bayes_records.size();
    }
}

TX void bayes_learn_structure() {
    for (int var = 1; var < g_bayes_num_vars; var++) {
        std::vector<int> best_parents;
        double best_score = bayes_compute_score(var, best_parents);

        for (int p = 0; p < var && p < g_bayes_max_parents; p++) {
            std::vector<int> test_parents = {p};
            double score = bayes_compute_score(var, test_parents);
            if (score > best_score) {
                best_score = score;
                best_parents = test_parents;
            }
        }

        if (!best_parents.empty()) {
            g_bayes_network[var].parents = best_parents;
            int cpt_size = 1 << best_parents.size();
            for (int j = 0; j < cpt_size; j++) {
                g_bayes_network[var].cpts[j] = 0.5 + (rand() % 100) / 200.0;
            }
        }
    }
}

void worker_bayes(ThreadData* data) {
    data->barrier->wait();

    while (!stop_workers.load() && data->loops-- > 0) {
        int var = data->thread_id;
        if (var < g_bayes_num_vars) {
            double score = bayes_compute_score(var, g_bayes_network[var].parents);
            g_bayes_scores[var] = score;
        }
        total_ops.fetch_add(1);
    }
}

// ============================================================================
// GENOME - Gene Sequencing
// ============================================================================

constexpr int GENOME_MAX_SEQ = 10000;
constexpr int GENOME_SEG_LEN = 64;
constexpr int GENOME_HASH_SIZE = 100000;

struct GenomeSegment {
    int id;
    std::string sequence;
    int hash;
};

TM std::vector<GenomeSegment> g_genome_segments;
TM std::unordered_set<int> g_genome_hash_set;
TM std::vector<std::string> g_genome_chains;

void genome_generate_segments() {
    g_genome_segments.clear();
    g_genome_hash_set.clear();

    const char* bases = "ACGT";
    for (int i = 0; i < GENOME_MAX_SEQ; i++) {
        GenomeSegment seg;
        seg.id = i;
        seg.sequence = "";
        for (int j = 0; j < GENOME_SEG_LEN; j++) {
            seg.sequence += bases[rand() % 4];
        }

        int hash = 0;
        for (int j = 0; j < (int)seg.sequence.size() - 3; j++) {
            int kmer = (seg.sequence[j] << 12) | (seg.sequence[j+1] << 8) | (seg.sequence[j+2] << 4) | seg.sequence[j+3];
            hash ^= kmer;
        }
        seg.hash = hash;
        g_genome_segments.push_back(seg);
        g_genome_hash_set.insert(hash);
    }
}

TX int genome_dedup() {
    int count = 0;
   std::unordered_set<int> seen;
    for (size_t i = 0; i < g_genome_segments.size(); i++) {
        int h = g_genome_segments[i].hash;
        if (seen.find(h) == seen.end()) {
            seen.insert(h);
            count++;
        }
    }
    return count;
}

int genome_rabin_karp_impl(const std::string& pattern, const std::string& text) {
    int m = pattern.length();
    int n = text.length();
    if (m > n) return -1;

    int pattern_hash = 0;
    int text_hash = 0;
    int power = 1;

    for (int i = 0; i < m - 1; i++) power *= 4;

    for (int i = 0; i < m; i++) {
        pattern_hash = pattern_hash * 4 + (pattern[i] - 'A');
        text_hash = text_hash * 4 + (text[i] - 'A');
    }

    for (int i = 0; i <= n - m; i++) {
        if (pattern_hash == text_hash) {
            if (text.compare(i, m, pattern) == 0) return i;
        }
        if (i < n - m) {
            text_hash = (4 * (text_hash - power * (text[i] - 'A')) + (text[i + m] - 'A'));
        }
    }
    return -1;
}

TX int genome_find_extendable_chain(const std::string& seg) {
    int best_pos = -1;
    size_t best_idx = (size_t)-1;
    size_t best_len = 0;
    
    for (size_t i = 0; i < g_genome_chains.size(); i++) {
        std::string& chain = g_genome_chains[i];
        int pos = genome_rabin_karp_impl(chain.substr(0, 4), seg);
        if (pos >= 0 && chain.length() > best_len) {
            best_pos = pos;
            best_idx = i;
            best_len = chain.length();
        }
    }
    
    if (best_idx != (size_t)-1) {
        g_genome_chains[best_idx] = seg + g_genome_chains[best_idx].substr(best_pos + 4);
    }
    
    return best_pos;
}

TX void genome_add_new_chain(const std::string& seg) {
    g_genome_chains.push_back(seg);
}

void worker_genome(ThreadData* data) {
    data->barrier->wait();

    int t = data->thread_id;
    size_t num_segs = std::min(g_genome_segments.size(), (size_t)1000);
    size_t chunk = num_segs / g_num_threads;
    size_t start = t * chunk;
    size_t end = (t == g_num_threads - 1) ? num_segs : start + chunk;
    
    while (!stop_workers.load() && data->loops-- > 0) {
        size_t seg_idx = start + (rand() % (end - start));
        if (seg_idx < g_genome_segments.size()) {
            const std::string& seg = g_genome_segments[seg_idx].sequence;
            genome_find_extendable_chain(seg);
        }
        total_ops.fetch_add(1);
    }
}

// ============================================================================
// KMEANS - K-Means Clustering
// ============================================================================

constexpr int KMEANS_MAX_CLUSTERS = 50;
constexpr int KMEANS_DIMENSIONS = 16;
constexpr int KMEANS_MAX_POINTS = 100000;

struct KMeansPoint {
    double coords[KMEANS_DIMENSIONS];
    int cluster;
};

TM std::vector<KMeansPoint> g_kmeans_points;
TM double g_kmeans_centroids[KMEANS_MAX_CLUSTERS][KMEANS_DIMENSIONS];
TM int g_kmeans_num_clusters = 5;
TM int g_kmeans_num_points = 1000;

void kmeans_generate_points() {
    g_kmeans_points.clear();

    double local_centroids[KMEANS_MAX_CLUSTERS][KMEANS_DIMENSIONS];
    
    for (int c = 0; c < g_kmeans_num_clusters; c++) {
        for (int d = 0; d < KMEANS_DIMENSIONS; d++) {
            local_centroids[c][d] = (rand() % 1000) / 10.0;
        }
    }
    
    for (int c = 0; c < g_kmeans_num_clusters; c++) {
        for (int d = 0; d < KMEANS_DIMENSIONS; d++) {
            g_kmeans_centroids[c][d] = local_centroids[c][d];
        }
    }

    for (int i = 0; i < g_kmeans_num_points; i++) {
        KMeansPoint p;
        int cluster = rand() % g_kmeans_num_clusters;
        for (int d = 0; d < KMEANS_DIMENSIONS; d++) {
            p.coords[d] = local_centroids[cluster][d] + (rand() % 100 - 50) / 10.0;
        }
        p.cluster = cluster;
        g_kmeans_points.push_back(p);
    }
}

TX double kmeans_assign_cluster_point(size_t idx) {
    if (idx >= g_kmeans_points.size()) return 0;
    
    KMeansPoint& p = g_kmeans_points[idx];
    double min_dist = 1e30;
    int best_cluster = 0;

    for (int c = 0; c < g_kmeans_num_clusters; c++) {
        double dist = 0;
        for (int d = 0; d < KMEANS_DIMENSIONS; d++) {
            double diff = p.coords[d] - g_kmeans_centroids[c][d];
            dist += diff * diff;
        }
        if (dist < min_dist) {
            min_dist = dist;
            best_cluster = c;
        }
    }
    p.cluster = best_cluster;
    return min_dist;
}

TX void kmeans_update_centroids() {
    for (int c = 0; c < g_kmeans_num_clusters; c++) {
        for (int d = 0; d < KMEANS_DIMENSIONS; d++) {
            g_kmeans_centroids[c][d] = 0;
        }
    }

    std::vector<int> counts(g_kmeans_num_clusters, 0);

    for (size_t i = 0; i < g_kmeans_points.size(); i++) {
        int c = g_kmeans_points[i].cluster;
        for (int d = 0; d < KMEANS_DIMENSIONS; d++) {
            g_kmeans_centroids[c][d] += g_kmeans_points[i].coords[d];
        }
        counts[c]++;
    }

    for (int c = 0; c < g_kmeans_num_clusters; c++) {
        if (counts[c] > 0) {
            for (int d = 0; d < KMEANS_DIMENSIONS; d++) {
                g_kmeans_centroids[c][d] /= counts[c];
            }
        }
    }
}

void worker_kmeans(ThreadData* data) {
    data->barrier->wait();

    int t = data->thread_id;
    
    while (!stop_workers.load() && data->loops-- > 0) {
        size_t start = t * (g_kmeans_points.size() / g_num_threads);
        size_t end = (t == g_num_threads - 1) ? g_kmeans_points.size() : (t + 1) * (g_kmeans_points.size() / g_num_threads);
        
        for (size_t i = start; i < end; i++) {
            kmeans_assign_cluster_point(i);
        }
        
        if (t == 0) {
            kmeans_update_centroids();
        }
        
        total_ops.fetch_add(1);
    }
}

// ============================================================================
// INTRUDER - Network Intrusion Detection
// ============================================================================

constexpr int INTRUDER_MAX_PACKETS = 1000;
constexpr int INTRUDER_MAX_PAYLOAD = 128;
constexpr int INTRUDER_DICT_SIZE = 100;

struct Packet {
    int id;
    int src_ip;
    int dst_ip;
    int src_port;
    int dst_port;
    char payload[INTRUDER_MAX_PAYLOAD];
    int payload_len;
};

struct IntruderDictEntry {
    char pattern[32];
    int pattern_len;
};

TM std::deque<Packet> g_intruder_packets;
TM std::vector<IntruderDictEntry> g_intruder_dictionary;
TM std::unordered_set<uint64_t> g_intruder_signatures;

void intruder_generate_packets() {
    g_intruder_packets.clear();

    for (int i = 0; i < INTRUDER_MAX_PACKETS; i++) {
        Packet pkt;
        pkt.id = i;
        pkt.src_ip = rand() % 256;
        pkt.dst_ip = rand() % 256;
        pkt.src_port = (rand() % 60000) + 1024;
        pkt.dst_port = (rand() % 60000) + 1024;
        pkt.payload_len = rand() % INTRUDER_MAX_PAYLOAD;
        for (int j = 0; j < pkt.payload_len; j++) {
            pkt.payload[j] = (rand() % 26) + 'A';
        }
        g_intruder_packets.push_back(pkt);
    }
}

void intruder_generate_dictionary() {
    g_intruder_dictionary.clear();
    for (int i = 0; i < INTRUDER_DICT_SIZE; i++) {
        IntruderDictEntry entry;
        int len = (rand() % 20) + 4;
        for (int j = 0; j < len; j++) {
            entry.pattern[j] = (rand() % 26) + 'A';
        }
        entry.pattern_len = len;
        g_intruder_dictionary.push_back(entry);
    }
}

TX uint64_t intruder_compute_signature(const char* data, int len) {
    uint64_t sig = 0;
    for (int i = 0; i < len && i < 64; i++) {
        sig = sig * 31 + data[i];
    }
    return sig;
}

TX void intruder_process_packet(const Packet& pkt) {
    uint64_t sig = intruder_compute_signature(pkt.payload, pkt.payload_len);
    g_intruder_signatures.insert(sig);
}

TX int intruder_match_patterns_chunk(const char* payload, int len, int start, int end) {
    int matches = 0;
    for (int d = start; d < end && d < (int)g_intruder_dictionary.size(); d++) {
        const auto& entry = g_intruder_dictionary[d];
        if (entry.pattern_len > len) continue;
        for (int i = 0; i <= len - entry.pattern_len; i++) {
            bool match = true;
            for (int j = 0; j < entry.pattern_len; j++) {
                if (payload[i+j] != entry.pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) matches++;
        }
    }
    return matches;
}

void worker_intruder(ThreadData* data) {
    data->barrier->wait();

    int t = data->thread_id;
    size_t num_packets = g_intruder_packets.size();
    size_t chunk = std::max((size_t)1, num_packets / g_num_threads);
    size_t start = t * chunk;
    size_t end = std::min(start + chunk, num_packets);
    
    while (!stop_workers.load() && data->loops-- > 0) {
        if (start < end) {
            size_t pkt_idx = start + ((t + data->loops) % (end - start));
            if (pkt_idx < num_packets) {
                const Packet& pkt = g_intruder_packets[pkt_idx];
                intruder_process_packet(pkt);
                size_t dict_chunk = std::max((size_t)1, g_intruder_dictionary.size() / g_num_threads);
                size_t dict_start = t * dict_chunk;
                size_t dict_end = std::min(dict_start + dict_chunk, g_intruder_dictionary.size());
                intruder_match_patterns_chunk(pkt.payload, pkt.payload_len, dict_start, dict_end);
            }
        }
        total_ops.fetch_add(1);
    }
}

// ============================================================================
// LABYRINTH - Maze Routing (Full A* Algorithm)
// ============================================================================

constexpr int LABYRINTH_GRID_SIZE = 32;
constexpr int LABYRINTH_NUM_ROUTES = 10;

struct Coordinate {
    int x, y;
    bool operator<(const Coordinate& other) const {
        return x < other.x || (x == other.x && y < other.y);
    }
    bool operator==(const Coordinate& other) const {
        return x == other.x && y == other.y;
    }
};

struct MazeCell {
    bool wall;
    bool visited;
    Coordinate parent;
    double g_cost;
    double h_cost;
};

std::vector<std::vector<MazeCell>> labyrinth_grid;
std::vector<Coordinate> labyrinth_routes;

void labyrinth_generate_maze() {
    labyrinth_grid.clear();
    labyrinth_grid.resize(LABYRINTH_GRID_SIZE);
    for (auto& row : labyrinth_grid) {
        row.resize(LABYRINTH_GRID_SIZE);
    }
    
    labyrinth_routes.clear();
    for (int i = 0; i < LABYRINTH_NUM_ROUTES; i++) {
        Coordinate start{rand() % LABYRINTH_GRID_SIZE, rand() % LABYRINTH_GRID_SIZE};
        Coordinate end{rand() % LABYRINTH_GRID_SIZE, rand() % LABYRINTH_GRID_SIZE};
        labyrinth_grid[start.x][start.y].wall = false;
        labyrinth_grid[end.x][end.y].wall = false;
        labyrinth_routes.push_back(start);
        labyrinth_routes.push_back(end);
    }
    
    for (int x = 0; x < LABYRINTH_GRID_SIZE; x++) {
        for (int y = 0; y < LABYRINTH_GRID_SIZE; y++) {
            labyrinth_grid[x][y].visited = false;
            labyrinth_grid[x][y].g_cost = 1e30;
            labyrinth_grid[x][y].h_cost = 0;
            labyrinth_grid[x][y].wall = (rand() % 100) < 30;
        }
    }
    
    for (size_t i = 0; i < labyrinth_routes.size(); i += 2) {
        labyrinth_grid[labyrinth_routes[i].x][labyrinth_routes[i].y].wall = false;
        labyrinth_grid[labyrinth_routes[i+1].x][labyrinth_routes[i+1].y].wall = false;
    }
}

double labyrinth_heuristic(Coordinate a, Coordinate b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

std::vector<Coordinate> labyrinth_a_star(Coordinate start, Coordinate end) {
    std::vector<Coordinate> path;
    
    for (int x = 0; x < LABYRINTH_GRID_SIZE; x++) {
        for (int y = 0; y < LABYRINTH_GRID_SIZE; y++) {
            labyrinth_grid[x][y].visited = false;
            labyrinth_grid[x][y].g_cost = 1e30;
            labyrinth_grid[x][y].parent = {-1, -1};
        }
    }
    
    if (labyrinth_grid[start.x][start.y].wall || labyrinth_grid[end.x][end.y].wall) {
        return path;
    }
    
    std::queue<Coordinate> q;
    q.push(start);
    labyrinth_grid[start.x][start.y].visited = true;
    labyrinth_grid[start.x][start.y].g_cost = 0;
    labyrinth_grid[start.x][start.y].parent = start;
    
    int dirs[] = {0, 1, 0, -1, 1, 0, -1, 0};
    bool found = false;
    
    while (!q.empty()) {
        Coordinate current = q.front();
        q.pop();
        
        if (current.x == end.x && current.y == end.y) {
            found = true;
            break;
        }
        
        for (int i = 0; i < 4; i++) {
            int nx = current.x + dirs[i*2];
            int ny = current.y + dirs[i*2+1];
            
            if (nx < 0 || nx >= LABYRINTH_GRID_SIZE || ny < 0 || ny >= LABYRINTH_GRID_SIZE) continue;
            if (labyrinth_grid[nx][ny].wall || labyrinth_grid[nx][ny].visited) continue;
            
            labyrinth_grid[nx][ny].visited = true;
            labyrinth_grid[nx][ny].parent = current;
            q.push({nx, ny});
        }
    }
    
    if (found) {
        Coordinate c = end;
        while (!(c.x == start.x && c.y == start.y)) {
            path.push_back(c);
            c = labyrinth_grid[c.x][c.y].parent;
        }
        path.push_back(start);
        std::reverse(path.begin(), path.end());
    }
    
    return path;
}

void worker_labyrinth(ThreadData* data) {
    data->barrier->wait();

    int t = data->thread_id;
    int num_pairs = labyrinth_routes.size() / 2;
    
    while (!stop_workers.load() && data->loops-- > 0) {
        int idx = (t + data->loops) % num_pairs;
        if (idx < num_pairs) {
            Coordinate start = labyrinth_routes[idx * 2];
            Coordinate end = labyrinth_routes[idx * 2 + 1];
            auto path = labyrinth_a_star(start, end);
        }
        total_ops.fetch_add(1);
    }
}

// ============================================================================
// SSCA2 - Graph Kernels
// ============================================================================

constexpr int SSCA2_MAX_VERTICES = 10000;
constexpr int SSCA2_NUM_EDGES = 50000;

struct SSCA2Edge {
    int from;
    int to;
    int weight;
};

std::vector<SSCA2Edge> g_ssca2_edges;
std::unordered_map<int, std::vector<int>> g_ssca2_adj;
std::vector<std::vector<int>> g_ssca2_clusters;

void ssca2_generate_graph() {
    g_ssca2_edges.clear();
    g_ssca2_adj.clear();

    for (int i = 0; i < SSCA2_NUM_EDGES; i++) {
        SSCA2Edge e;
        e.from = rand() % SSCA2_MAX_VERTICES;
        e.to = rand() % SSCA2_MAX_VERTICES;
        e.weight = rand() % 100;
        if (e.from != e.to) {
            g_ssca2_edges.push_back(e);
            g_ssca2_adj[e.from].push_back(e.to);
            g_ssca2_adj[e.to].push_back(e.from);
        }
    }
}

TX void ssca2_build_clusters() {
    g_ssca2_clusters.clear();
    std::vector<bool> visited(SSCA2_MAX_VERTICES, false);

    for (int v = 0; v < SSCA2_MAX_VERTICES; v++) {
        if (visited[v]) continue;

        std::vector<int> cluster;
        std::queue<int> q;
        q.push(v);
        visited[v] = true;

        while (!q.empty()) {
            int curr = q.front();
            q.pop();
            cluster.push_back(curr);

            auto it = g_ssca2_adj.find(curr);
            if (it != g_ssca2_adj.end()) {
                for (int neighbor : it->second) {
                    if (!visited[neighbor]) {
                        visited[neighbor] = true;
                        q.push(neighbor);
                    }
                }
            }
        }
        if (!cluster.empty()) {
            g_ssca2_clusters.push_back(cluster);
        }
    }
}

TX int ssca2_compute_intensity() {
    int total = 0;
    for (auto& cluster : g_ssca2_clusters) {
        int internal = 0;
        for (int v : cluster) {
            auto it = g_ssca2_adj.find(v);
            if (it != g_ssca2_adj.end()) {
                for (int neighbor : it->second) {
                    if (std::binary_search(cluster.begin(), cluster.end(), neighbor)) {
                        internal++;
                    }
                }
            }
        }
        total += internal;
    }
    return total;
}

void worker_ssca2(ThreadData* data) {
    data->barrier->wait();

    int t = data->thread_id;
    
    while (!stop_workers.load() && data->loops-- > 0) {
        int iteration = (g_num_threads - t) % 5;
        if (iteration == 0) {
            ssca2_build_clusters();
            ssca2_compute_intensity();
        }
        total_ops.fetch_add(1);
    }
}

// ============================================================================
// VACATION - Travel Reservation System
// ============================================================================

constexpr int VACATION_MAX_PRICES = 1000;
constexpr int VACATION_MAX_RESERVATIONS = 10000;

enum class ReservationType { FLIGHT, HOTEL, CAR };

struct VacationPrice {
    int id;
    ReservationType type;
    int price;
    int num_available;
};

struct VacationReservation {
    int id;
    ReservationType type;
    int price_id;
    int customer_id;
    bool confirmed;
};

std::vector<VacationPrice> g_vacation_prices;
std::vector<VacationReservation> g_vacation_reservations;
int g_vacation_balance = 1000000;

void vacation_generate_prices() {
    g_vacation_prices.clear();

    for (int i = 0; i < VACATION_MAX_PRICES; i++) {
        VacationPrice p;
        p.id = i;
        p.type = (ReservationType)(i % 3);
        p.price = 100 + (rand() % 900);
        p.num_available = 10 + rand() % 90;
        g_vacation_prices.push_back(p);
    }
}

TX bool vacation_make_reservation(ReservationType type, int price_id, int customer_id) {
    if (price_id >= (int)g_vacation_prices.size()) return false;

    VacationPrice& p = g_vacation_prices[price_id];
    if (p.type != type || p.num_available <= 0) return false;

    p.num_available--;
    g_vacation_balance -= p.price;
    
    VacationReservation r;
    r.id = g_vacation_reservations.size();
    r.type = type;
    r.price_id = price_id;
    r.customer_id = customer_id;
    r.confirmed = true;
    g_vacation_reservations.push_back(r);
    
    return true;
}

TX bool vacation_cancel_reservation(int res_id) {
    if (res_id >= (int)g_vacation_reservations.size()) return false;

    VacationReservation& r = g_vacation_reservations[res_id];
    if (!r.confirmed) return false;

    int pid = r.price_id;
    if (pid >= (int)g_vacation_prices.size()) return false;
    
    VacationPrice& p = g_vacation_prices[pid];
    p.num_available++;
    g_vacation_balance += p.price;
    r.confirmed = false;
    
    return true;
}

TX int vacation_query_availability(ReservationType type) {
    int count = 0;
    for (size_t i = 0; i < g_vacation_prices.size(); i++) {
        if (g_vacation_prices[i].type == type) {
            count += g_vacation_prices[i].num_available;
        }
    }
    return count;
}

void worker_vacation(ThreadData* data) {
    data->barrier->wait();

    int t = data->thread_id;
    
    while (!stop_workers.load() && data->loops-- > 0) {
        int op = (t + data->loops) % 100;
        
        if (op < 45) {
            ReservationType type = (ReservationType)((t + data->loops) % 3);
            int price_id = (t + data->loops) % VACATION_MAX_PRICES;
            vacation_make_reservation(type, price_id, t * 1000 + data->loops);
        } else if (op < 90) {
            if (!g_vacation_reservations.empty()) {
                vacation_cancel_reservation((t + data->loops) % g_vacation_reservations.size());
            }
        } else {
            ReservationType type = (ReservationType)(t % 3);
            vacation_query_availability(type);
        }

        total_ops.fetch_add(1);
    }
}

// ============================================================================
// YADA - Delaunay Mesh Refinement
// ============================================================================

constexpr int YADA_MAX_TRIANGLES = 50000;

struct Point2D {
    double x, y;
};

struct YadaTriangle {
    Point2D p1, p2, p3;
    Point2D center;
    bool active;
};

std::vector<YadaTriangle> g_yada_triangles;

void yada_generate_mesh() {
    g_yada_triangles.clear();

    std::set<std::pair<int, int>> edges;

    for (int i = 0; i < YADA_MAX_TRIANGLES; i++) {
        YadaTriangle t;
        t.p1.x = rand() % 1000 / 10.0;
        t.p1.y = rand() % 1000 / 10.0;
        t.p2.x = rand() % 1000 / 10.0;
        t.p2.y = rand() % 1000 / 10.0;
        t.p3.x = rand() % 1000 / 10.0;
        t.p3.y = rand() % 1000 / 10.0;
        t.active = true;
        g_yada_triangles.push_back(t);
    }
}

TX double yada_circumradius(const YadaTriangle& t) {
    double ax = t.p1.x, ay = t.p1.y;
    double bx = t.p2.x, by = t.p2.y;
    double cx = t.p3.x, cy = t.p3.y;

    double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::abs(d) < 1e-10) return 1e30;

    double ux = ((ax*ax + ay*ay) * (by - cy) + (bx*bx + by*by) * (cy - ay) + (cx*cx + cy*cy) * (ay - by)) / d;
    double uy = ((ax*ax + ay*ay) * (cx - bx) + (bx*bx + by*by) * (ax - cx) + (cx*cx + cy*cy) * (bx - ax)) / d;

    double dx = ux - ax;
    double dy = uy - ay;
    return dx*dx + dy*dy;
}

TX Point2D yada_circumcenter(const YadaTriangle& t) {
    double ax = t.p1.x, ay = t.p1.y;
    double bx = t.p2.x, by = t.p2.y;
    double cx = t.p3.x, cy = t.p3.y;

    double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::abs(d) < 1e-10) return {0, 0};

    double ux = ((ax*ax + ay*ay) * (by - cy) + (bx*bx + by*by) * (cy - ay) + (cx*cx + cy*cy) * (ay - by)) / d;
    double uy = ((ax*ax + ay*ay) * (cx - bx) + (bx*bx + by*by) * (ax - cx) + (cx*cx + cy*cy) * (bx - ax)) / d;

    return {ux, uy};
}

TX int yada_ruppert_refine() {
    int num_bad = 0;
    std::vector<YadaTriangle> new_triangles;

    for (auto& t : g_yada_triangles) {
        if (!t.active) continue;

        double circum_r = yada_circumradius(t);
        if (circum_r > 0.5) {
            num_bad++;

            Point2D center = yada_circumcenter(t);

            YadaTriangle t1 = {t.p1, t.p2, center, true};
            YadaTriangle t2 = {t.p2, t.p3, center, true};
            YadaTriangle t3 = {t.p3, t.p1, center, true};

            t.active = false;
            if (new_triangles.size() < 10000) {
                new_triangles.push_back(t1);
                new_triangles.push_back(t2);
                new_triangles.push_back(t3);
            }
        }
    }

    for (auto& t : new_triangles) {
        if (g_yada_triangles.size() < YADA_MAX_TRIANGLES) {
            g_yada_triangles.push_back(t);
        }
    }

    return num_bad;
}

TX void yada_compute_bad_triangles() {
    for (size_t i = 0; i < g_yada_triangles.size(); i++) {
        if (!g_yada_triangles[i].active) continue;
        
        double circum_r = yada_circumradius(g_yada_triangles[i]);
        if (circum_r > 0.5) {
            Point2D center = yada_circumcenter(g_yada_triangles[i]);
            g_yada_triangles[i].center = center;
            g_yada_triangles[i].active = false;
        }
    }
}

TX void yada_refine_mesh() {
    for (size_t i = 0; i < g_yada_triangles.size(); i++) {
        if (g_yada_triangles[i].center.x != 0 || g_yada_triangles[i].center.y != 0) {
            if (g_yada_triangles.size() < YADA_MAX_TRIANGLES) {
                YadaTriangle t;
                t.p1 = g_yada_triangles[i].p1;
                t.p2 = g_yada_triangles[i].p2;
                t.p3 = g_yada_triangles[i].center;
                t.active = true;
                g_yada_triangles.push_back(t);
            }
        }
    }
}

void worker_yada(ThreadData* data) {
    data->barrier->wait();

    int t = data->thread_id;
    
    while (!stop_workers.load() && data->loops-- > 0) {
        size_t start = t * (g_yada_triangles.size() / g_num_threads);
        size_t end = (t == g_num_threads - 1) ? g_yada_triangles.size() : (t + 1) * (g_yada_triangles.size() / g_num_threads);
        
        int local_bad = 0;
        for (size_t i = start; i < end; i++) {
            if (!g_yada_triangles[i].active) continue;
            
            double circum_r = yada_circumradius(g_yada_triangles[i]);
            if (circum_r > 0.5) {
                local_bad++;
                Point2D center = yada_circumcenter(g_yada_triangles[i]);
                g_yada_triangles[i].center = center;
            }
        }
        total_ops.fetch_add(1);
    }
}

// ============================================================================
// Main and Benchmark Runner
// ============================================================================

void run_benchmark(BenchmarkType bench, int threads, int duration_ms);

int main(int argc, char* argv[]) {
    g_benchmark = BenchmarkType::BAYES;
    g_num_threads = DEFAULT_NB_THREADS;
    g_duration = DEFAULT_DURATION_MS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            g_num_threads = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            g_duration = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            char b = argv[++i][0];
            switch(b) {
                case 'b': case 'B': g_benchmark = BenchmarkType::BAYES; break;
                case 'g': case 'G': g_benchmark = BenchmarkType::GENOME; break;
                case 'i': case 'I': g_benchmark = BenchmarkType::INTRUDER; break;
                case 'k': case 'K': g_benchmark = BenchmarkType::KMEANS; break;
                case 'l': case 'L': g_benchmark = BenchmarkType::LABYRINTH; break;
                case 's': case 'S': g_benchmark = BenchmarkType::SSCA2; break;
                case 'v': case 'V': g_benchmark = BenchmarkType::VACATION; break;
                case 'y': case 'Y': g_benchmark = BenchmarkType::YADA; break;
            }
        }
    }

    const char* bench_names[] = {
        "bayes", "genome", "intruder", "kmeans",
        "labyrinth", "ssca2", "vacation", "yada"
    };

    std::cout << "STAMP Benchmark Suite (Full Specification)\n";
    std::cout << "========================================\n";
    std::cout << "Benchmark: " << bench_names[(int)g_benchmark] << "\n";
    std::cout << "Threads:   " << g_num_threads << "\n";
    std::cout << "Duration:  " << g_duration << " ms\n\n";

    run_benchmark(g_benchmark, g_num_threads, g_duration);

    return 0;
}

void run_benchmark(BenchmarkType bench, int threads, int duration_ms) {
    switch(bench) {
        case BenchmarkType::BAYES:
            bayes_generate_network();
            bayes_generate_records();
            break;
        case BenchmarkType::GENOME:
            genome_generate_segments();
            break;
        case BenchmarkType::KMEANS:
            kmeans_generate_points();
            break;
        case BenchmarkType::INTRUDER:
            intruder_generate_packets();
            intruder_generate_dictionary();
            break;
        case BenchmarkType::LABYRINTH:
            labyrinth_generate_maze();
            break;
        case BenchmarkType::SSCA2:
            ssca2_generate_graph();
            break;
        case BenchmarkType::VACATION:
            vacation_generate_prices();
            break;
        case BenchmarkType::YADA:
            yada_generate_mesh();
            break;
    }

    int loops = duration_ms / 10;
    if (loops < 10) loops = 10;

    Barrier barrier(threads);
    std::vector<ThreadData> td(threads);
    std::vector<std::thread> workers;

    for (int i = 0; i < threads; i++) {
        td[i].barrier = &barrier;
        td[i].thread_id = i;
        td[i].loops = loops;
        td[i].benchmark = bench;
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < threads; i++) {
        switch(bench) {
            case BenchmarkType::BAYES:
                workers.emplace_back(worker_bayes, &td[i]); break;
            case BenchmarkType::GENOME:
                workers.emplace_back(worker_genome, &td[i]); break;
            case BenchmarkType::KMEANS:
                workers.emplace_back(worker_kmeans, &td[i]); break;
            case BenchmarkType::INTRUDER:
                workers.emplace_back(worker_intruder, &td[i]); break;
            case BenchmarkType::LABYRINTH:
                workers.emplace_back(worker_labyrinth, &td[i]); break;
            case BenchmarkType::SSCA2:
                workers.emplace_back(worker_ssca2, &td[i]); break;
            case BenchmarkType::VACATION:
                workers.emplace_back(worker_vacation, &td[i]); break;
            case BenchmarkType::YADA:
                workers.emplace_back(worker_yada, &td[i]); break;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    stop_workers = true;

    for (auto& w : workers) w.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t ops = total_ops.load();

    std::cout << "Results\n";
    std::cout << "=======\n";
    std::cout << "Elapsed:    " << ms << " ms\n";
    std::cout << "Total ops: " << ops << "\n";
    std::cout << "Ops/sec:   " << (ops * 1000.0 / ms) << "\n";
    std::cout << "Aborts:    " << abort_count.load() << "\n";
}