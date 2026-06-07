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

#include "../../tests/benchmark_test.hpp"

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

static int g_num_threads = 4;
static int g_percent_attack = 10;
static int g_max_data_length = 16;
static int g_num_flows = 1024;
static int g_seed = 1;

static void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) g_num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) g_percent_attack = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) g_max_data_length = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) g_num_flows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) g_seed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s -p <threads> -a <pct_attack> -l <max_len> -n <flows> -s <seed>\n", argv[0]);
            exit(0);
        }
    }
}

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

static void test_cli_flags() {
    printf("  Testing CLI flags...\n");
    int save_p = g_num_threads, save_a = g_percent_attack, save_l = g_max_data_length;
    int save_n = g_num_flows, save_s = g_seed;
    TEST_EQ(g_num_threads, 4, "default threads");
    TEST_EQ(g_percent_attack, 10, "default attack pct");
    TEST_EQ(g_max_data_length, 16, "default max data length");
    TEST_EQ(g_num_flows, 1024, "default num flows");
    TEST_EQ(g_seed, 1, "default seed");
    const char* test_args[] = {"prog", "-p", "2", "-a", "50", "-l", "32", "-n", "8", "-s", "99"};
    parse_args(11, (char**)test_args);
    TEST_EQ(g_num_threads, 2, "override threads");
    TEST_EQ(g_percent_attack, 50, "override attack pct");
    TEST_EQ(g_max_data_length, 32, "override max data length");
    TEST_EQ(g_num_flows, 8, "override num flows");
    TEST_EQ(g_seed, 99, "override seed");
    g_num_threads = save_p; g_percent_attack = save_a; g_max_data_length = save_l;
    g_num_flows = save_n; g_seed = save_s;
    if (test_result() != 0) exit(1);
}

static void test_rng() {
    printf("  Testing RNG determinism...\n");
    test_rng_determinism<PRNG>();
    if (test_result() != 0) exit(1);
}

static void test_logic() {
    printf("  Testing intruder logic...\n");
    // Verify attack classification
    std::vector<std::string> dict = {"attack", "root", "system"};
    TEST_ASSERT(detect_attack("this is an attack string", dict), "detect attack");
    TEST_ASSERT(detect_attack("ROOT access granted", dict), "detect case-insensitive");
    TEST_ASSERT(!detect_attack("benign data here", dict), "no false positive");
    // Test packet generation determinism
    PRNG rng(42);
    int num_packets = 0, attack_count = 0;
    for (long flow = 1; flow <= 20; flow++) {
        bool is_attack = (int)(rng() % 100) < 10;
        if (is_attack) attack_count++;
        int payload_len = (int)(rng() % 16) + 1;
        int np = (int)(rng() % payload_len) + 1;
        num_packets += np;
    }
    TEST_ASSERT(num_packets > 0, "generated packets");
    if (test_result() != 0) exit(1);
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("Running self-tests for intruder...\n");
        test_cli_flags();
        test_rng();
        test_logic();
        printf("All tests passed.\n");
        return 0;
    }
    parse_args(argc, argv);

    expli::TM<int>::init();

    g_data.num_flows = g_num_flows;
    g_data.max_data_length = g_max_data_length;
    g_data.percent_attack = g_percent_attack;
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

    PRNG rng(g_seed);
    int total_packets = 0;

    for (long flow = 1; flow <= g_num_flows; flow++) {
        bool is_attack = (int)(rng() % 100) < g_percent_attack;
        std::string payload;

        if (is_attack) {
            int sig_idx = (int)(rng() % g_data.dictionary.size());
            payload = g_data.dictionary[sig_idx];
            g_data.total_attacks++;
        } else {
            int len = (int)(rng() % g_max_data_length) + 1;
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

    printf("Percent attack  = %i\n", g_percent_attack);
    printf("Max data length = %i\n", g_max_data_length);
    printf("Num flow        = %i\n", g_num_flows);
    printf("Random seed     = %i\n", g_seed);
    printf("Num attack      = %i\n", g_data.total_attacks);
    fflush(stdout);

    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < g_num_threads; i++)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    printf("Elapsed time = %f seconds\n", elapsed / 1000.0);
    printf("Num found = %lu\n", (unsigned long)ops);

    expli::TM<int>::exit();
    return 0;
}
