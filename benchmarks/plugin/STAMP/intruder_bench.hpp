#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#define INTRUDER_MAX_PACKETS 64
#define INTRUDER_MAX_DATA 256

struct Packet {
    long flow_id;
    int fragment_id;
    int num_fragments;
    int length;
    char data[INTRUDER_MAX_DATA];
};

struct DecodedFlow {
    long flow_id;
    int num_fragments_received;
    char data[INTRUDER_MAX_DATA * 2];
    int data_len;
};

struct TM IntruderData {
    Packet* packet_queue;
    int packet_q_head;
    int packet_q_tail;
    int packet_q_capacity;

    DecodedFlow* decoder_flows;
    char* fragment_storage;
    int* fragment_counts;

    DecodedFlow* decoded_queue;
    int decoded_q_head;
    int decoded_q_tail;
    int decoded_q_capacity;

    char** dictionary;
    int dictionary_size;
    int num_flows;
    int max_data_length;
    int percent_attack;
    int total_attacks;
    int total_packets;
};

static IntruderData* g_intruder = nullptr;

inline void intruder_generate_packets() {
    auto data = tm_new<IntruderData>();
    data->num_flows = g_intruder_n;
    data->max_data_length = g_intruder_l;
    data->percent_attack = g_intruder_a;
    data->total_attacks = 0;

    std::vector<std::string> dict_vec = {
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
    data->dictionary_size = (int)dict_vec.size();
    data->dictionary = new char*[data->dictionary_size];
    for (int i = 0; i < data->dictionary_size; i++) {
        data->dictionary[i] = new char[dict_vec[i].size() + 1];
        std::strcpy(data->dictionary[i], dict_vec[i].c_str());
    }

    int packet_capacity = g_intruder_n * INTRUDER_MAX_PACKETS + 1024;
    data->packet_queue = tm_new_array<Packet>(packet_capacity);
    data->packet_q_head = 0;
    data->packet_q_tail = 0;
    data->packet_q_capacity = packet_capacity;

    data->decoder_flows = tm_new_array<DecodedFlow>(g_intruder_n);
    data->fragment_storage = (char*)tm_calloc(g_intruder_n * INTRUDER_MAX_PACKETS * INTRUDER_MAX_DATA, 1);
    data->fragment_counts = (int*)tm_calloc(g_intruder_n, sizeof(int));

    data->decoded_queue = tm_new_array<DecodedFlow>(g_intruder_n);
    data->decoded_q_head = 0;
    data->decoded_q_tail = 0;
    data->decoded_q_capacity = g_intruder_n;

    PRNG rng(g_intruder_s);
    int total_packets = 0;

    for (long flow = 1; flow <= data->num_flows; flow++) {
        bool is_attack = (int)(rng.next() % 100) < data->percent_attack;
        std::string payload;

        if (is_attack) {
            int sig_idx = (int)(rng.next() % data->dictionary_size);
            payload = dict_vec[sig_idx];
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
            Packet& pkt = data->packet_queue[data->packet_q_tail];
            pkt.flow_id = flow;
            pkt.fragment_id = f;
            pkt.num_fragments = num_packets;
            int this_len = base_len + (f < rem ? 1 : 0);
            pkt.length = this_len;
            std::strncpy(pkt.data, payload.c_str() + offset, this_len);
            pkt.data[this_len] = '\0';
            offset += this_len;
            data->packet_q_tail++;
            total_packets++;
        }
    }

    data->total_packets = total_packets;
    g_intruder = data;

    printf("Percent attack  = %i\n", data->percent_attack);
    printf("Max data length = %i\n", data->max_data_length);
    printf("Num flow        = %i\n", data->num_flows);
    printf("Random seed     = %i\n", g_intruder_s);
    printf("Num attack      = %i\n", data->total_attacks);
    fflush(stdout);
}

// Check if decoded payload matches any dictionary entry (signature match).
// In the original benchmark, this is the core detection logic.
static inline bool detect_attack(const char* data, int data_len,
                                  char** dictionary, int dict_size) {
    for (int i = 0; i < dict_size; i++) {
        int dlen = (int)std::strlen(dictionary[i]);
        if (data_len >= dlen &&
            std::strncmp(data + data_len - dlen, dictionary[i], dlen) == 0)
            return true;
    }
    return false;
}

TX static Packet get_packet(IntruderData* data) {
    if (data->packet_q_head >= data->packet_q_tail) {
        Packet empty;
        empty.flow_id = -1;
        return empty;
    }
    Packet p = data->packet_queue[data->packet_q_head];
    data->packet_q_head++;
    return p;
}

__attribute__((annotate("tm_allow_opaque")))
TX static void process_decoder(IntruderData* data, const Packet& pkt) {
    int flow_idx = (int)(pkt.flow_id - 1);
    DecodedFlow& df = data->decoder_flows[flow_idx];
    df.flow_id = pkt.flow_id;
    df.num_fragments_received++;

    int fc = data->fragment_counts[flow_idx];
    char* frag = &data->fragment_storage[
        (flow_idx * INTRUDER_MAX_PACKETS + fc) * INTRUDER_MAX_DATA];
    tm_memcpy(frag, pkt.data, pkt.length);
    frag[pkt.length] = '\0';
    data->fragment_counts[flow_idx] = fc + 1;

    if (df.num_fragments_received == pkt.num_fragments) {
        int total = 0;
        for (int i = 0; i < pkt.num_fragments; i++) {
            char* src = &data->fragment_storage[
                (flow_idx * INTRUDER_MAX_PACKETS + i) * INTRUDER_MAX_DATA];
            int slen = 0;
            while (slen < INTRUDER_MAX_DATA && src[slen]) slen++;
            tm_memcpy(df.data + total, src, slen);
            total += slen;
        }
        df.data_len = total;
        df.data[total] = '\0';

        data->decoded_queue[data->decoded_q_tail] = df;
        data->decoded_q_tail++;

        df.flow_id = -1;
        df.num_fragments_received = 0;
        data->fragment_counts[flow_idx] = 0;
    }
}

TX static DecodedFlow get_complete(IntruderData* data) {
    if (data->decoded_q_head >= data->decoded_q_tail) {
        DecodedFlow empty;
        empty.flow_id = -1;
        return empty;
    }
    DecodedFlow df = data->decoded_queue[data->decoded_q_head];
    data->decoded_q_head++;
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
            bool attack = detect_attack(df.data, df.data_len,
                                         data->dictionary, data->dictionary_size);
            (void)attack;
            total_ops.fetch_add(1, std::memory_order_relaxed);
        } else if (pkt.flow_id < 0) {
            break;
        }
    }
}
