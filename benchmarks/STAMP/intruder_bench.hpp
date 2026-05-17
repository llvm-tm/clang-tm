#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

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

struct TM IntruderData {
    std::queue<Packet> packet_queue;
    std::unordered_map<long, DecodedFlow> decoder_map;
    std::queue<DecodedFlow> decoded_queue;
    std::vector<std::string> dictionary;
    int num_flows;
    int max_data_length;
    int percent_attack;
    int total_attacks;
};

static IntruderData* g_intruder = nullptr;

inline void intruder_generate_packets() {
    auto data = new IntruderData();
    data->num_flows = g_intruder_n;
    data->max_data_length = g_intruder_l;
    data->percent_attack = g_intruder_a;
    data->total_attacks = 0;

    data->dictionary = {"about", "attack", "back", "root", "system", "access",
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

    PRNG rng(g_intruder_s);
    int total_packets = 0;

    for (long flow = 1; flow <= data->num_flows; flow++) {
        bool is_attack = (int)(rng.next() % 100) < data->percent_attack;
        std::string payload;

        if (is_attack) {
            int sig_idx = (int)(rng.next() % data->dictionary.size());
            payload = data->dictionary[sig_idx];
            data->total_attacks++;
        } else {
            int len = (int)(rng.next() % data->max_data_length) + 1;
            payload.resize(len);
            for (int i = 0; i < len; i++) {
                payload[i] = (char)(32 + (rng.next() % 95));
            }
        }

        int num_packets = (int)(rng.next() % (int)payload.length()) + 1;
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
            data->packet_queue.push(pkt);
            total_packets++;
        }
    }

    (void)total_packets;
    g_intruder = data;
}

static inline bool detect_attack(const std::string& data, const std::vector<std::string>& dict) {
    std::string lower;
    lower.resize(data.size());
    for (size_t i = 0; i < data.size(); i++) {
        lower[i] = (data[i] >= 'A' && data[i] <= 'Z') ? (data[i] - 'A' + 'a') : data[i];
    }
    for (const auto& sig : dict) {
        if (lower.find(sig) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TX static Packet get_packet(IntruderData* data) {
    if (data->packet_queue.empty()) return {-1, -1, -1, -1, ""};
    Packet p = data->packet_queue.front();
    data->packet_queue.pop();
    return p;
}

TX static void process_decoder(IntruderData* data, const Packet& pkt) {
    auto& decoder_map = data->decoder_map;
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
        for (int i = 0; i < pkt.num_fragments; i++) {
            full += it->second.fragments[i];
        }
        it->second.data = full;
        data->decoded_queue.push(it->second);
        decoder_map.erase(it);
    }
}

TX static DecodedFlow get_complete(IntruderData* data) {
    if (data->decoded_queue.empty()) return {-1, "", 0, {}};
    DecodedFlow df = data->decoded_queue.front();
    data->decoded_queue.pop();
    return df;
}

THREAD void worker_intruder(ThreadData* td) {
    auto data = g_intruder;

    for (;;) {
        Packet pkt = get_packet(data);
        if (pkt.flow_id >= 0) {
            process_decoder(data, pkt);
        }
        DecodedFlow df = get_complete(data);
        if (df.flow_id >= 0) {
            bool attack = detect_attack(df.data, data->dictionary);
            (void)attack;
            total_ops.fetch_add(1, std::memory_order_relaxed);
        } else if (pkt.flow_id < 0) {
            break;
        }
    }
}
