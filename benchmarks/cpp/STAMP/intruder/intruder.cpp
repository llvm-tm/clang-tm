// STAMP/intruder benchmark — explicit TM API port
// Replaces std::mutex serialization with TM transactions on pre-allocated
// flat arrays (no STL containers in TM path).  Modeled on the plugin
// intruder_bench.hpp data structures.
//
// WHY FLAT ARRAYS: std::queue / std::unordered_map / std::string do
// operator-new internally, which the TM runtime intercepts and cannot
// safely roll back on abort.  Pre-allocated POD arrays avoid this.
//
// See also: benchmarks/plugin/STAMP/intruder_bench.hpp (plugin version)
//           benchmarks/rust/src/stamp/intruder.rs (rust version)

#include "expli_tm_api/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "../../tests/benchmark_test.hpp"

using PRNG = std::mt19937_64;

// ── Fixed-size limits (match plugin) ────────────────────────────────
static const int INTRUDER_MAX_DATA    = 256;
static const int INTRUDER_MAX_PACKETS = 128;

// ── POD data structures (no std::string, no std::vector) ────────────
struct Packet {
    long flow_id;
    int  fragment_id;
    int  num_fragments;
    int  length;
    char data[INTRUDER_MAX_DATA];
};

struct DecodedFlow {
    long flow_id;
    int  num_fragments_received;
    char data[INTRUDER_MAX_DATA * 2];
    int  data_len;
};

// ── Global TM-controlled state (all in TM region) ────────────────────
// Packet queue (monotonic — head/tail only advance)
static Packet*  g_packet_queue        = nullptr;
static int*     g_packet_q_head       = nullptr;
static int*     g_packet_q_tail       = nullptr;
static int      g_packet_q_capacity   = 0;

// Per-flow decoder state
static DecodedFlow* g_decoder_flows  = nullptr;  // [max_flow_id]
static char*        g_fragment_storage = nullptr; // [flow][packet][INTRUDER_MAX_DATA]
static int*         g_fragment_counts = nullptr;  // [flow]

// Decoded flow queue (monotonic)
static DecodedFlow* g_decoded_queue  = nullptr;
static int*    g_decoded_q_head       = nullptr;
static int*    g_decoded_q_tail       = nullptr;
static int     g_decoded_q_capacity   = 0;

// Dictionary (read-only after init)
static char** g_dictionary           = nullptr;
static int    g_dictionary_size      = 0;

static int g_num_threads  = 4;
static int g_percent_attack = 10;
static int g_max_data_length = 16;
static int g_num_flows    = 1024;
static int g_seed         = 1;

static std::atomic<uint64_t> g_total_ops{0};
static void parse_args(int argc, char* argv[]);

// ── TM wrapper (uses tm_api.hpp declarations with proper types) ──────
extern __thread int32_t tm_nested_call_counter;
extern __thread sigjmp_buf tm_jmpbuf;

template<typename F>
static void tx_run(F&& body) {
    volatile bool done = false;
    while (!done) {
        sigsetjmp(tm_jmpbuf, 0);
        tm_nested_call_counter = 1;
        tm_begin();
        body();
        tm_end();
        done = true;
    }
    tm_nested_call_counter = 0;
}

// ── TX: dequeue a packet ────────────────────────────────────────────
static Packet get_packet() {
    Packet pkt;
    pkt.flow_id = -1;
    tx_run([&]() {
        int h = (int)tm_read_i4((uint32_t*)g_packet_q_head);
        int t = (int)tm_read_i4((uint32_t*)g_packet_q_tail);
        if (h < t) {
            Packet* src = &g_packet_queue[h];
            pkt.flow_id       = (long)tm_read_i8((uint64_t*)&src->flow_id);
            pkt.fragment_id   = (int)tm_read_i4((uint32_t*)&src->fragment_id);
            pkt.num_fragments = (int)tm_read_i4((uint32_t*)&src->num_fragments);
            pkt.length        = (int)tm_read_i4((uint32_t*)&src->length);
            for (int i = 0; i < INTRUDER_MAX_DATA; i++)
                pkt.data[i] = (char)tm_read_i1((uint8_t*)&src->data[i]);
            tm_write_i4((uint32_t*)g_packet_q_head, h + 1);
        }
    });
    return pkt;
}

// ── TX: reassemble a fragment into the decoder flow ─────────────────
static void process_decoder(const Packet& pkt) {
    tx_run([&]() {
        long fid = pkt.flow_id;
        int  idx = (int)(fid - 1);
        long storage_off = (fid - 1) * INTRUDER_MAX_PACKETS * INTRUDER_MAX_DATA
                          + pkt.fragment_id * INTRUDER_MAX_DATA;

        for (int i = 0; i < pkt.length && i < INTRUDER_MAX_DATA; i++)
            tm_write_i1((uint8_t*)&g_fragment_storage[storage_off + i],
                       (uint8_t)pkt.data[i]);

        int prev = (int)tm_read_i4((uint32_t*)&g_fragment_counts[idx]);
        tm_write_i4((uint32_t*)&g_fragment_counts[idx], prev + 1);

        if (prev + 1 == pkt.num_fragments) {
            int total = 0;
            for (int f = 0; f < pkt.num_fragments; f++) {
                long off = (fid - 1) * INTRUDER_MAX_PACKETS * INTRUDER_MAX_DATA
                          + f * INTRUDER_MAX_DATA;
                int flen = pkt.length;
                for (int i = 0; i < flen && total < INTRUDER_MAX_DATA * 2; i++)
                    tm_write_i1((uint8_t*)&g_decoder_flows[idx].data[total++],
                               tm_read_i1((uint8_t*)&g_fragment_storage[off + i]));
            }
            tm_write_i4((uint32_t*)&g_decoder_flows[idx].data_len, total);
            tm_write_i8((uint64_t*)&g_decoder_flows[idx].flow_id, fid);

            int qtail = (int)tm_read_i4((uint32_t*)g_decoded_q_tail);
            if (qtail < g_decoded_q_capacity) {
                DecodedFlow* dst = &g_decoded_queue[qtail];
                tm_write_i8((uint64_t*)&dst->flow_id, fid);
                tm_write_i4((uint32_t*)&dst->data_len, total);
                for (int i = 0; i < total; i++)
                    tm_write_i1((uint8_t*)&dst->data[i],
                               g_decoder_flows[idx].data[i]);
                tm_write_i4((uint32_t*)g_decoded_q_tail, qtail + 1);
            }
            tm_write_i4((uint32_t*)&g_fragment_counts[idx], 0);
        }
    });
}

// ── TX: dequeue a completed decoded flow ────────────────────────────
static DecodedFlow get_complete() {
    DecodedFlow df;
    df.flow_id = -1;
    tx_run([&]() {
        int h = (int)tm_read_i4((uint32_t*)g_decoded_q_head);
        int t = (int)tm_read_i4((uint32_t*)g_decoded_q_tail);
        if (h < t) {
            DecodedFlow* src = &g_decoded_queue[h];
            df.flow_id  = (long)tm_read_i8((uint64_t*)&src->flow_id);
            df.data_len = (int)tm_read_i4((uint32_t*)&src->data_len);
            for (int i = 0; i < df.data_len && i < INTRUDER_MAX_DATA * 2; i++)
                df.data[i] = (char)tm_read_i1((uint8_t*)&src->data[i]);
            tm_write_i4((uint32_t*)g_decoded_q_head, h + 1);
        }
    });
    return df;
}

// ── Attack detection (non-TX, pure char* ops) ───────────────────────
static bool detect_attack(const char* data, int data_len) {
    // Case-insensitive substring search against dictionary
    char lower[INTRUDER_MAX_DATA * 2 + 1];
    int  len = data_len < INTRUDER_MAX_DATA * 2 ? data_len : INTRUDER_MAX_DATA * 2;
    for (int i = 0; i < len; i++)
        lower[i] = (data[i] >= 'A' && data[i] <= 'Z') ? (data[i] - 'A' + 'a') : data[i];
    lower[len] = '\0';

    for (int d = 0; d < g_dictionary_size; d++) {
        if (strstr(lower, g_dictionary[d]) != nullptr)
            return true;
    }
    return false;
}

// ── Worker thread ───────────────────────────────────────────────────
static void worker() {
    tm_init_thread();

    for (;;) {
        Packet pkt = get_packet();
        if (pkt.flow_id >= 0)
            process_decoder(pkt);

        DecodedFlow df = get_complete();
        if (df.flow_id >= 0) {
            bool attack = detect_attack(df.data, df.data_len);
            (void)attack;
            g_total_ops.fetch_add(1, std::memory_order_relaxed);
        } else if (pkt.flow_id < 0) {
            break;
        }
    }

    tm_exit_thread();
}

// ── Self-tests ──────────────────────────────────────────────────────
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
    // Verify attack detection on char* data
    const char* dict[] = {"attack", "root", "system"};
    g_dictionary = const_cast<char**>(dict);
    g_dictionary_size = 3;
    TEST_ASSERT(detect_attack("this is an attack string", 24), "detect attack");
    TEST_ASSERT(detect_attack("ROOT access granted", 19), "detect case-insensitive");
    TEST_ASSERT(!detect_attack("benign data here", 16), "no false positive");

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

// ── Main ────────────────────────────────────────────────────────────
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

    // ── Initialise TM runtime ──
    tm_init();

    // ── Build dictionary (read-only after init, no TM needed) ──
    const char* wordlist[] = {
        "about", "attack", "back", "root", "system", "access",
        "all", "after", "also", "and", "any", "are", "but",
        "can", "come", "could", "did", "do", "each", "find",
        "first", "for", "from", "get", "go", "has", "have",
        "her", "here", "him", "his", "how", "into", "its",
        "just", "know", "like", "look", "make", "man", "may",
        "more", "most", "must", "new", "no", "not", "now",
        "old", "one", "only", "other", "our", "out", "over",
        "own", "part", "people", "said", "say", "see", "she",
        "shell", "should", "site", "some", "such", "take",
        "than", "that", "their", "them", "then", "there"
    };
    g_dictionary_size = (int)(sizeof(wordlist) / sizeof(wordlist[0]));
    g_dictionary = new char*[g_dictionary_size];
    for (int i = 0; i < g_dictionary_size; i++) {
        g_dictionary[i] = new char[strlen(wordlist[i]) + 1];
        strcpy(g_dictionary[i], wordlist[i]);
    }

    // ── Allocate TM-safe flat arrays ──
    int max_flows = g_num_flows;
    int max_packets_per_flow = g_max_data_length;  // each byte → one fragment max
    if (max_packets_per_flow > INTRUDER_MAX_PACKETS)
        max_packets_per_flow = INTRUDER_MAX_PACKETS;

    // Packet queue: worst-case g_num_flows * max_packets_per_flow
    g_packet_q_capacity = max_flows * max_packets_per_flow + 1024;
    g_packet_queue      = (Packet*)tm_calloc(g_packet_q_capacity, sizeof(Packet));
    g_packet_q_head     = (int*)tm_calloc(1, sizeof(int));
    g_packet_q_tail     = (int*)tm_calloc(1, sizeof(int));

    // Per-flow decoder state
    g_decoder_flows     = (DecodedFlow*)tm_calloc(max_flows, sizeof(DecodedFlow));
    g_fragment_storage  = (char*)tm_calloc(max_flows * INTRUDER_MAX_PACKETS * INTRUDER_MAX_DATA, 1);
    g_fragment_counts   = (int*)tm_calloc(max_flows, sizeof(int));

    // Decoded queue: at most one per flow
    g_decoded_q_capacity = max_flows;
    g_decoded_queue      = (DecodedFlow*)tm_calloc(g_decoded_q_capacity, sizeof(DecodedFlow));
    g_decoded_q_head     = (int*)tm_calloc(1, sizeof(int));
    g_decoded_q_tail     = (int*)tm_calloc(1, sizeof(int));

    // ── Generate packets (main thread, no TM) ──
    int total_packets = 0;
    {
        PRNG rng(g_seed);
        int attack_count = 0;

        for (long flow = 1; flow <= max_flows; flow++) {
            bool is_attack = (int)(rng() % 100) < g_percent_attack;
            char payload[INTRUDER_MAX_DATA * 2];
            int  payload_len;

            if (is_attack) {
                int sig_idx = (int)(rng() % g_dictionary_size);
                int slen = (int)strlen(g_dictionary[sig_idx]);
                for (int i = 0; i < slen && i < INTRUDER_MAX_DATA * 2; i++)
                    payload[i] = g_dictionary[sig_idx][i];
                payload_len = slen;
                attack_count++;
            } else {
                payload_len = (int)(rng() % g_max_data_length) + 1;
                if (payload_len > INTRUDER_MAX_DATA * 2)
                    payload_len = INTRUDER_MAX_DATA * 2;
                for (int i = 0; i < payload_len; i++)
                    payload[i] = (char)(32 + (rng() % 95));
            }

            int num_packets = (int)(rng() % payload_len) + 1;
            if (num_packets < 1) num_packets = 1;
            if (num_packets > INTRUDER_MAX_PACKETS)
                num_packets = INTRUDER_MAX_PACKETS;

            int base_len = payload_len / num_packets;
            int rem      = payload_len % num_packets;

            int offset = 0;
            for (int f = 0; f < num_packets; f++) {
                Packet& pkt = g_packet_queue[(*g_packet_q_tail)++];
                pkt.flow_id       = flow;
                pkt.fragment_id   = f;
                pkt.num_fragments = num_packets;
                int this_len = base_len + (f < rem ? 1 : 0);
                pkt.length = this_len;
                for (int i = 0; i < this_len && i < INTRUDER_MAX_DATA; i++)
                    pkt.data[i] = payload[offset + i];
                offset += this_len;
                total_packets++;
            }
        }
        (void)attack_count;
    }

    printf("Percent attack  = %i\n", g_percent_attack);
    printf("Max data length = %i\n", g_max_data_length);
    printf("Num flow        = %i\n", g_num_flows);
    printf("Random seed     = %i\n", g_seed);
    printf("Total packets   = %i\n", total_packets);
    fflush(stdout);

    // ── Run workers ──
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

    // ── Cleanup ──
    tm_free(g_packet_queue);
    tm_free(g_packet_q_head);
    tm_free(g_packet_q_tail);
    tm_free(g_decoder_flows);
    tm_free(g_fragment_storage);
    tm_free(g_fragment_counts);
    tm_free(g_decoded_queue);
    tm_free(g_decoded_q_head);
    tm_free(g_decoded_q_tail);
    for (int i = 0; i < g_dictionary_size; i++)
        delete[] g_dictionary[i];
    delete[] g_dictionary;

    tm_exit();
    return 0;
}
