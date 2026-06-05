// STAMP/intruder benchmark — explicit TM API port
// Matches the plugin intruder_bench.hpp algorithm.
//
// Uses std:: containers with mutex for serialization, matching the
// plugin's tm_serialize_lock/unlock inside TX functions.

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <queue>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <mutex>
#include <random>

using PRNG = std::mt19937_64;

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

struct IntruderData {
    std::queue<Packet> packet_queue;
    std::unordered_map<long, DecodedFlow> decoder_map;
    std::queue<DecodedFlow> decoded_queue;
    std::vector<std::string> dictionary;
    int num_flows;
    int max_data_length;
    int percent_attack;
    int total_attacks;
};

static IntruderData g_data;
static std::mutex g_mutex;          // matches tm_serialize_lock

// ── Attack detection (non-TX, pure string ops) ────────────
static bool detect_attack(const std::string& data, const std::vector<std::string>& dict) {
    std::string lower;
    lower.resize(data.size());
    for (size_t i = 0; i < data.size(); i++)
        lower[i] = (data[i] >= 'A' && data[i] <= 'Z') ? (data[i] - 'A' + 'a') : data[i];
    for (const auto& sig : dict) {
        if (lower.find(sig) != std::string::npos)
            return true;
    }
    return false;
}

// ── Serialized queue/decode operations ────────────────────
static Packet get_packet() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_data.packet_queue.empty()) return {-1, -1, -1, -1, ""};
    Packet p = g_data.packet_queue.front();
    g_data.packet_queue.pop();
    return p;
}

static void process_decoder(const Packet& pkt) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& decoder_map = g_data.decoder_map;
    auto it = decoder_map.find(pkt.flow_id);
    if (it == decoder_map.end()) {
        DecodedFlow df;
        df.flow_id = pkt.flow_id;
        df.num_fragments_received = 0;
        df.fragments.resize(pkt.num_fragments);
        decoder_map[pkt.flow_id] = df;
        it = decoder_map.find(pkt.flow_id);
    }

    it->second.fragments[pkt.fragment_id] = pkt.data;
    it->second.num_fragments_received++;

    if (it->second.num_fragments_received == pkt.num_fragments) {
        std::string full;
        for (int i = 0; i < pkt.num_fragments; i++)
            full += it->second.fragments[i];
        it->second.data = full;
        g_data.decoded_queue.push(it->second);
        decoder_map.erase(it);
    }
}

static DecodedFlow get_complete() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_data.decoded_queue.empty()) return {-1, "", 0, {}};
    DecodedFlow df = g_data.decoded_queue.front();
    g_data.decoded_queue.pop();
    return df;
}

// ── Worker thread ─────────────────────────────────────────
static std::atomic<uint64_t> g_total_ops{0};

static void worker() {
    expli::TM<int>::thread_init();

    for (;;) {
        Packet pkt = get_packet();
        if (pkt.flow_id >= 0)
            process_decoder(pkt);

        DecodedFlow df = get_complete();
        if (df.flow_id >= 0) {
            bool attack = detect_attack(df.data, g_data.dictionary);
            (void)attack;
            g_total_ops.fetch_add(1, std::memory_order_relaxed);
        } else if (pkt.flow_id < 0) {
            break;
        }
    }

    expli::TM<int>::thread_exit();
}

int main(int argc, char* argv[]) {
    int num_threads = 4;
    int percent_attack = 10;
    int max_data_length = 128;
    int num_flows = 1048576;
    int seed = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) percent_attack = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) max_data_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) num_flows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -a <pct_attack> -l <max_len> -n <flows> -s <seed>\n", argv[0]);
            return 0;
        }
    }

    expli::TM<int>::init();

    g_data.num_flows = num_flows;
    g_data.max_data_length = max_data_length;
    g_data.percent_attack = percent_attack;
    g_data.total_attacks = 0;

    g_data.dictionary = {"about", "attack", "back", "root", "system", "access",
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

    PRNG rng(seed);
    int total_packets = 0;

    for (long flow = 1; flow <= num_flows; flow++) {
        bool is_attack = (int)(rng() % 100) < percent_attack;
        std::string payload;

        if (is_attack) {
            int sig_idx = (int)(rng() % g_data.dictionary.size());
            payload = g_data.dictionary[sig_idx];
            g_data.total_attacks++;
        } else {
            int len = (int)(rng() % max_data_length) + 1;
            payload.resize(len);
            for (int i = 0; i < len; i++)
                payload[i] = (char)(32 + (rng() % 95));
        }

        int num_packets = (int)(rng() % (int)payload.length()) + 1;
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
            pkt.data = payload.substr(offset, this_len);
            offset += this_len;
            g_data.packet_queue.push(pkt);
            total_packets++;
        }
    }

    (void)total_packets;

    printf("Percent attack  = %i\n", percent_attack);
    printf("Max data length = %i\n", max_data_length);
    printf("Num flow        = %i\n", num_flows);
    printf("Random seed     = %i\n", seed);
    printf("Num attack      = %i\n", g_data.total_attacks);
    fflush(stdout);

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    printf("    Time = %lld ms\n", (long long)elapsed);
    printf("    Completed flows = %llu / %d\n", (unsigned long long)ops, num_flows);

    expli::TM<int>::exit();
    return 0;
}
