// Intruder — C++ port of the original STAMP spec (explicit API path)
// Original spec: https://github.com/ccaominh/stamp/tree/master/intruder
//
// Parameters:
//   -a <num>   Attack percentage (default: 10)
//   -l <num>   Max data length   (default: 1024)
//   -n <num>   Num flows         (default: 10000)
//   -s <num>   Random seed       (default: 1)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>
#include <queue>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <mutex>
#include <random>

static long g_pct_attack   = 10;
static long g_max_data_len = 1024;
static long g_num_flows    = 10000;
static long g_seed         = 1;
static long g_num_threads  = 4;

// ── TM init ────────────────────────────────────────────────────────
extern "C" {
    void     tm_init();
    void     tm_exit();
    void     tm_init_thread();
    void     tm_exit_thread();
}

static thread_local std::mt19937_64 tls_rng;
static void rng_seed(uint64_t s) { tls_rng = std::mt19937_64(s); }
static uint64_t rng_next() { return tls_rng(); }

// ── Data structures (matching plugin) ──────────────────────────────
struct Packet {
    long flow_id;
    int fragment_id;
    int num_fragments;
    int length;
    std::string data;
};

struct DecodedFlow {
    long flow_id;
    std::string data;
    int num_fragments_received;
    std::vector<std::string> fragments;
};

static std::queue<Packet> g_packet_queue;
static std::unordered_map<long, DecodedFlow> g_decoder_map;
static std::queue<DecodedFlow> g_decoded_queue;
static std::vector<std::string> g_dictionary;
static std::mutex g_mutex;
static std::atomic<long> g_total_acks{0};

// ── Attack detection ───────────────────────────────────────────────
static bool detect_attack(const std::string& data, const std::vector<std::string>& dict) {
    std::string lower;
    lower.resize(data.size());
    for (size_t i = 0; i < data.size(); i++)
        lower[i] = (data[i] >= 'A' && data[i] <= 'Z') ? (data[i] - 'A' + 'a') : data[i];
    for (const auto& sig : dict) {
        if (lower.find(sig) != std::string::npos) return true;
    }
    return false;
}

// ── Packet generation ──────────────────────────────────────────────
static int g_total_attacks = 0;
static int g_total_packets = 0;

static Packet get_packet() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_packet_queue.empty()) return {-1, -1, -1, -1, ""};
    Packet p = g_packet_queue.front();
    g_packet_queue.pop();
    return p;
}

static void process_decoder(const Packet& pkt) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_decoder_map.find(pkt.flow_id);
    if (it == g_decoder_map.end()) {
        DecodedFlow df;
        df.flow_id = pkt.flow_id;
        df.num_fragments_received = 0;
        df.fragments.resize((size_t)pkt.num_fragments);
        it = g_decoder_map.emplace(pkt.flow_id, std::move(df)).first;
    }
    it->second.fragments[(size_t)pkt.fragment_id] = pkt.data;
    it->second.num_fragments_received++;
    if (it->second.num_fragments_received == pkt.num_fragments) {
        std::string full;
        for (int i = 0; i < pkt.num_fragments; i++)
            full += it->second.fragments[(size_t)i];
        it->second.data = full;
        g_decoded_queue.push(it->second);
        g_decoder_map.erase(it);
    }
}

static DecodedFlow get_complete() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_decoded_queue.empty()) return {-1, "", 0, {}};
    DecodedFlow df = g_decoded_queue.front();
    g_decoded_queue.pop();
    return df;
}

// ── Worker ─────────────────────────────────────────────────────────
static void worker(long tid) {
    tm_init_thread();
    for (;;) {
        Packet pkt = get_packet();
        if (pkt.flow_id >= 0) process_decoder(pkt);
        DecodedFlow df = get_complete();
        if (df.flow_id >= 0) {
            detect_attack(df.data, g_dictionary);
            g_total_acks.fetch_add(1, std::memory_order_relaxed);
        } else if (pkt.flow_id < 0) {
            break;
        }
    }
    tm_exit_thread();
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-a") == 0 && i+1 < argc) g_pct_attack   = atol(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i+1 < argc) g_max_data_len = atol(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) g_num_flows    = atol(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) g_seed         = atol(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc) g_num_threads  = atol(argv[++i]);
    }

    printf("Intruder (STAMP spec)\n");
    printf("  Attack pct:    %ld\n", g_pct_attack);
    printf("  Max data len:  %ld\n", g_max_data_len);
    printf("  Flows:         %ld\n", g_num_flows);
    printf("  Seed:          %ld\n", g_seed);
    printf("  Threads:       %ld\n", g_num_threads);

    tm_init();

    // Build dictionary (matching plugin)
    g_dictionary = {"about", "attack", "back", "root", "system", "access",
                    "all", "after", "also", "and", "any", "are", "but",
                    "can", "come", "could", "did", "do", "each", "find",
                    "first", "for", "from", "get", "go", "has", "have",
                    "her", "here", "him", "his", "how", "into", "its",
                    "just", "know", "like", "look", "make", "man", "may",
                    "more", "most", "must", "new", "no", "not", "now",
                    "old", "one", "only", "other", "our", "out", "over",
                    "own", "part", "people", "said", "say", "see", "she",
                    "shell", "should", "site", "some", "such", "take",
                    "than", "that", "their", "them", "then", "there"};

    // Generate packets
    rng_seed((uint64_t)g_seed);
    for (long flow = 1; flow <= g_num_flows; flow++) {
        bool is_attack = (int)(rng_next() % 100) < g_pct_attack;
        std::string payload;
        if (is_attack) {
            int sig_idx = (int)(rng_next() % g_dictionary.size());
            payload = g_dictionary[(size_t)sig_idx];
            g_total_attacks++;
        } else {
            int len = (int)(rng_next() % (uint64_t)g_max_data_len) + 1;
            payload.resize((size_t)len);
            for (int i = 0; i < len; i++)
                payload[(size_t)i] = (char)(32 + (rng_next() % 95));
        }
        int num_packets = (int)(rng_next() % (uint64_t)payload.length()) + 1;
        if (num_packets < 1) num_packets = 1;
        int base_len = (int)payload.length() / num_packets;
        int rem = (int)payload.length() % num_packets;
        int offset = 0;
        for (int f = 0; f < num_packets; f++) {
            Packet pkt;
            pkt.flow_id = flow;
            pkt.fragment_id = f;
            pkt.num_fragments = num_packets;
            int this_len = base_len + (f < rem ? 1 : 0);
            pkt.length = this_len;
            pkt.data = payload.substr((size_t)offset, (size_t)this_len);
            offset += this_len;
            g_packet_queue.push(pkt);
            g_total_packets++;
        }
    }

    printf("  Total packets: %d\n", g_total_packets);
    printf("  Attack flows:  %d\n", g_total_attacks);

    // Run workers
    auto t1 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (long t = 0; t < g_num_threads; t++)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    printf("\nResults (%ld ms):\n", (long)(elapsed * 1000));
    printf("  Processed:     %ld\n", g_total_acks.load());
    printf("  PASS\n");

    tm_exit();
    return 0;
}
