/**
 * STMbench7 — OPTIMIZED variant
 *
 * Uses TM_LOCAL annotations on local variables inside TX functions
 * to reduce TM instrumentation overhead.
 *
 * Based on: "STMBench7: A Benchmark for Software Transactional Memory"
 *   Guerraoui, Kapalka, Vitek. EuroSys 2007.
 */

#include "../../backends/tm_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int FANOUT           = 3;
constexpr int TREE_LEVELS      = 6;
constexpr int MAX_MODULES      = 1;
constexpr int MAX_CP           = 500;
constexpr int AP_PER_CP        = 200;
constexpr int CONN_PER_AP      = 3;
constexpr int MAX_DOCUMENTS    = MAX_CP;
constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS  = 4;

constexpr int MAX_CA = ([]() constexpr {
    int total = 0, level_size = 1;
    for (int l = 0; l < TREE_LEVELS; l++) {
        total += level_size;
        level_size *= FANOUT;
    }
    return total;
})();
constexpr int MAX_BA = ([]() constexpr {
    int ba = 1;
    for (int l = 0; l < TREE_LEVELS; l++) ba *= FANOUT;
    return ba;
})();
constexpr int MAX_AP     = MAX_CP * AP_PER_CP;
constexpr int MAX_CONN   = MAX_AP * CONN_PER_AP;
constexpr int MAX_CP_BA_BAG = 5;

struct Manual {
    int id;
    char text[256];
};

struct Document {
    int id;
    int type;
    int buildDate;
    int compositePartId;
};

struct Connection {
    int id;
    int fromAtomicPartId;
    int toAtomicPartId;
    int type;
};

struct AtomicPart {
    int id;
    int x, y, z;
    int buildDate;
    int weight;
    int compositePartId;
    std::vector<int> connectionIds;
};

struct CompositePart {
    int id;
    int buildDate;
    int documentId;
    int rootAtomicPartId;
    std::vector<int> atomicPartIds;
    std::vector<int> baseAssemblyIds;
};

struct BaseAssembly {
    int id;
    int parentAssemblyId;
    int buildDate;
    std::vector<int> compositePartIds;
};

struct ComplexAssembly {
    int id;
    int level;
    int parentId;
    std::vector<int> childAssemblyIds;
    std::vector<int> childBaseAssemblyIds;
    int buildDate;
};

struct Module {
    int id;
    int rootAssemblyId;
};

TM std::vector<Module>          g_modules;
TM std::vector<ComplexAssembly> g_complexAssemblies;
TM std::vector<BaseAssembly>    g_baseAssemblies;
TM std::vector<CompositePart>   g_compositeParts;
TM std::vector<AtomicPart>      g_atomicParts;
TM std::vector<Connection>      g_connections;
TM std::vector<Document>        g_documents;
TM Manual                       g_manual;

TM std::map<int, int>           g_caById;
TM std::map<int, int>           g_baById;
TM std::map<int, int>           g_cpById;
TM std::map<int, int>           g_apById;
TM std::map<int, int>           g_docById;
TM std::multimap<int, int>      g_cpByDate;
TM std::multimap<int, int>      g_apByDate;

TM int g_atomicPartCount = 0;
TM int g_connectionCount = 0;

static void init_data() {
    TM_LOCAL int level_sizes[TREE_LEVELS];
    TM_LOCAL int level_offset[TREE_LEVELS];

    g_modules.clear();
    g_complexAssemblies.clear();
    g_baseAssemblies.clear();
    g_compositeParts.clear();
    g_atomicParts.clear();
    g_connections.clear();
    g_documents.clear();
    g_caById.clear();
    g_baById.clear();
    g_cpById.clear();
    g_apById.clear();
    g_docById.clear();
    g_cpByDate.clear();
    g_apByDate.clear();

    Module mod;
    mod.id = 0;
    mod.rootAssemblyId = 0;
    g_modules.push_back(mod);

    {
        TM_LOCAL int off = 0;
        TM_LOCAL int sz  = 1;
        for (TM_LOCAL int l = 0; l < TREE_LEVELS; l++) {
            level_sizes[l]  = sz;
            level_offset[l] = off;
            for (TM_LOCAL int j = 0; j < sz; j++) {
                ComplexAssembly ca;
                ca.id        = off + j;
                ca.level     = l;
                ca.parentId  = -1;
                ca.buildDate = 1000 + (l * 100 + j) % 365;
                g_complexAssemblies.push_back(ca);
            }
            off += sz;
            sz  *= FANOUT;
        }
    }

    for (TM_LOCAL int l = 1; l < TREE_LEVELS; l++) {
        TM_LOCAL int parent_off  = level_offset[l - 1];
        TM_LOCAL int parent_sz   = level_sizes[l - 1];
        TM_LOCAL int child_off   = level_offset[l];
        for (TM_LOCAL int p = 0; p < parent_sz; p++) {
            TM_LOCAL int parent_idx = parent_off + p;
            for (TM_LOCAL int c = 0; c < FANOUT; c++) {
                TM_LOCAL int child_idx = child_off + p * FANOUT + c;
                g_complexAssemblies[parent_idx].childAssemblyIds.push_back(child_idx);
                g_complexAssemblies[child_idx].parentId = parent_idx;
            }
        }
    }

    {
        TM_LOCAL int ba_parent_off = level_offset[TREE_LEVELS - 1];
        TM_LOCAL int ba_parent_sz  = level_sizes[TREE_LEVELS - 1];
        TM_LOCAL int ba_id = 0;
        for (TM_LOCAL int p = 0; p < ba_parent_sz; p++) {
            TM_LOCAL int parent_idx = ba_parent_off + p;
            for (TM_LOCAL int c = 0; c < FANOUT; c++) {
                BaseAssembly ba;
                ba.id                 = ba_id;
                ba.parentAssemblyId    = parent_idx;
                ba.buildDate          = 1000 + (ba_id % 365);
                g_baseAssemblies.push_back(ba);
                g_complexAssemblies[parent_idx].childBaseAssemblyIds.push_back(ba_id);
                ba_id++;
            }
        }
    }

    for (TM_LOCAL int cp_idx = 0; cp_idx < MAX_CP; cp_idx++) {
        CompositePart cp;
        cp.id            = cp_idx;
        cp.buildDate     = 1000 + (cp_idx % 365);
        cp.documentId    = cp_idx;
        cp.rootAtomicPartId = -1;

        Document doc;
        doc.id              = cp_idx;
        doc.type            = cp_idx % 3;
        doc.buildDate       = cp.buildDate;
        doc.compositePartId = cp_idx;
        g_documents.push_back(doc);

        g_compositeParts.push_back(cp);
        g_cpById[cp.id] = cp_idx;
        g_cpByDate.insert({cp.buildDate, cp_idx});
    }

    {
        std::mt19937 rng(42);
        for (TM_LOCAL int cp_idx = 0; cp_idx < MAX_CP; cp_idx++) {
            TM_LOCAL int num_ba = 1 + (rng() % (MAX_CP_BA_BAG - 1));
            std::set<int> chosen;
            while ((int)chosen.size() < num_ba)
                chosen.insert(rng() % MAX_BA);
            for (TM_LOCAL int ba_idx : chosen) {
                g_compositeParts[cp_idx].baseAssemblyIds.push_back(ba_idx);
                g_baseAssemblies[ba_idx].compositePartIds.push_back(cp_idx);
            }
        }
    }

    {
        std::mt19937 rng(99);
        for (TM_LOCAL int cp_idx = 0; cp_idx < MAX_CP; cp_idx++) {
            TM_LOCAL int first_ap_idx = (int)g_atomicParts.size();
            g_compositeParts[cp_idx].rootAtomicPartId = first_ap_idx;

            for (TM_LOCAL int j = 0; j < AP_PER_CP; j++) {
                AtomicPart ap;
                ap.id              = g_atomicParts.size();
                ap.x               = j % 100;
                ap.y               = (j / 100) % 100;
                ap.z               = j / 10000;
                ap.buildDate        = 1000 + (cp_idx * AP_PER_CP + j) % 365;
                ap.weight           = (j % 50) + 1;
                ap.compositePartId  = cp_idx;
                g_atomicParts.push_back(ap);
                g_compositeParts[cp_idx].atomicPartIds.push_back(ap.id);
                g_apById[ap.id] = (int)g_atomicParts.size() - 1;
                g_apByDate.insert({ap.buildDate, (int)g_atomicParts.size() - 1});
            }

            for (TM_LOCAL int j = 0; j < AP_PER_CP; j++) {
                TM_LOCAL int ap_idx = first_ap_idx + j;
                TM_LOCAL int t1 = first_ap_idx + (j + 1) % AP_PER_CP;
                Connection c1;
                c1.id               = g_connections.size();
                c1.fromAtomicPartId = ap_idx;
                c1.toAtomicPartId   = t1;
                c1.type             = j % 3;
                g_connections.push_back(c1);
                g_atomicParts[ap_idx].connectionIds.push_back(c1.id);

                TM_LOCAL int t2 = first_ap_idx + (j + 2) % AP_PER_CP;
                Connection c2;
                c2.id               = g_connections.size();
                c2.fromAtomicPartId = ap_idx;
                c2.toAtomicPartId   = t2;
                c2.type             = (j + 1) % 3;
                g_connections.push_back(c2);
                g_atomicParts[ap_idx].connectionIds.push_back(c2.id);

                for (TM_LOCAL int k = 0; k < CONN_PER_AP - 2; k++) {
                    TM_LOCAL int t3 = first_ap_idx + (rng() % AP_PER_CP);
                    if (t3 == ap_idx) { k--; continue; }
                    Connection c3;
                    c3.id               = g_connections.size();
                    c3.fromAtomicPartId = ap_idx;
                    c3.toAtomicPartId   = t3;
                    c3.type             = (j + k) % 3;
                    g_connections.push_back(c3);
                    g_atomicParts[ap_idx].connectionIds.push_back(c3.id);
                }
            }
        }
    }

    g_manual.id = 0;
    strncpy(g_manual.text, "STMbench7 Manual", sizeof(g_manual.text) - 1);
    g_manual.text[sizeof(g_manual.text) - 1] = '\0';

    g_atomicPartCount = (int)g_atomicParts.size();
    g_connectionCount = (int)g_connections.size();
}

static int pick_ca(std::mt19937 &rng) { return rng() % (int)g_complexAssemblies.size(); }
static int pick_ba(std::mt19937 &rng) { return rng() % (int)g_baseAssemblies.size(); }
static int pick_cp(std::mt19937 &rng) { return rng() % (int)g_compositeParts.size(); }
static int pick_ap(std::mt19937 &rng) { return rng() % (int)g_atomicParts.size(); }
static int pick_doc(std::mt19937 &rng) { return rng() % (int)g_documents.size(); }

struct OpResult {
    uint64_t value;
    bool     wasWrite;
};

// ─── LONG TRAVERSALS ──────────────────────────────────────────────────

TX OpResult op_lt1() {
    TM_LOCAL int64_t sum = 0;
    for (TM_LOCAL auto &ca : g_complexAssemblies) sum += ca.id + ca.level + ca.buildDate;
    for (TM_LOCAL auto &ba : g_baseAssemblies)   sum += ba.id + ba.buildDate;
    return { (uint64_t)sum, false };
}

TX OpResult op_lt2() {
    for (TM_LOCAL auto &ca : g_complexAssemblies) ca.buildDate = (ca.buildDate + 1) % 365 + 1000;
    for (TM_LOCAL auto &ba : g_baseAssemblies)   ba.buildDate = (ba.buildDate + 1) % 365 + 1000;
    return { 0, true };
}

TX OpResult op_lt3() {
    TM_LOCAL int64_t sum = 0;
    for (TM_LOCAL auto &cp : g_compositeParts) sum += cp.id + cp.buildDate;
    return { (uint64_t)sum, false };
}

TX OpResult op_lt4() {
    TM_LOCAL int64_t sum = 0;
    for (TM_LOCAL auto &ap : g_atomicParts) {
        ap.weight = (ap.weight % 50) + 1;
        sum += ap.weight;
    }
    return { (uint64_t)sum, true };
}

TX OpResult op_lt5() {
    TM_LOCAL int64_t sum = 0;
    for (TM_LOCAL auto &c : g_connections) sum += c.id + c.fromAtomicPartId + c.toAtomicPartId + c.type;
    return { (uint64_t)sum, false };
}

// ─── SHORT TRAVERSALS ────────────────────────────────────────────────

TX OpResult op_st1() {
    TM_LOCAL int64_t sum = 0;
    if (g_modules.empty()) return { 0, false };
    TM_LOCAL int ca_idx = g_modules[0].rootAssemblyId;
    if (ca_idx < 0 || ca_idx >= (int)g_complexAssemblies.size()) return { 0, false };
    sum += g_complexAssemblies[ca_idx].id;
    for (TM_LOCAL int l = 0; l < TREE_LEVELS - 1; l++) {
        TM_LOCAL auto &ca = g_complexAssemblies[ca_idx];
        if (ca.childAssemblyIds.empty()) break;
        ca_idx = ca.childAssemblyIds[0];
        sum += g_complexAssemblies[ca_idx].id;
    }
    TM_LOCAL auto &last_ca = g_complexAssemblies[ca_idx];
    if (!last_ca.childBaseAssemblyIds.empty()) {
        TM_LOCAL int ba_idx = last_ca.childBaseAssemblyIds[0];
        if (ba_idx < (int)g_baseAssemblies.size()) {
            TM_LOCAL auto &ba = g_baseAssemblies[ba_idx];
            sum += ba.id + ba.buildDate;
            if (!ba.compositePartIds.empty()) {
                TM_LOCAL int cp_idx = ba.compositePartIds[0];
                if (cp_idx < (int)g_compositeParts.size())
                    sum += g_compositeParts[cp_idx].buildDate;
            }
        }
    }
    return { (uint64_t)sum, false };
}

TX OpResult op_st2_traverse(int doc_idx) {
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return {0, false};
    TM_LOCAL int64_t sum = g_documents[doc_idx].buildDate + g_documents[doc_idx].type;
    TM_LOCAL int cp_idx = g_documents[doc_idx].compositePartId;
    if (cp_idx >= 0 && cp_idx < (int)g_compositeParts.size()) {
        for (TM_LOCAL int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
            if (ap_idx >= 0 && ap_idx < (int)g_atomicParts.size())
                sum += g_atomicParts[ap_idx].weight;
        }
    }
    return { (uint64_t)sum, false };
}

TX OpResult op_st3_traverse(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, false };
    TM_LOCAL int64_t sum = 0;
    TM_LOCAL auto &ap = g_atomicParts[ap_idx];
    sum += ap.x + ap.y + ap.z;
    for (TM_LOCAL int conn_idx : ap.connectionIds) {
        if (conn_idx < (int)g_connections.size()) {
            TM_LOCAL auto &c = g_connections[conn_idx];
            TM_LOCAL int nb = (c.fromAtomicPartId == ap_idx) ? c.toAtomicPartId : c.fromAtomicPartId;
            if (nb >= 0 && nb < (int)g_atomicParts.size())
                sum += g_atomicParts[nb].weight;
        }
    }
    return { (uint64_t)sum, false };
}

TX OpResult op_st4_update_ca(int ca_idx) {
    if (ca_idx < 0 || ca_idx >= (int)g_complexAssemblies.size()) return { 0, true };
    g_complexAssemblies[ca_idx].buildDate = (g_complexAssemblies[ca_idx].buildDate + 1) % 365 + 1000;
    return { 1, true };
}

TX OpResult op_st5_date_range(int low, int high) {
    TM_LOCAL int64_t count = 0;
    TM_LOCAL auto it = g_cpByDate.lower_bound(low);
    while (it != g_cpByDate.end() && it->first <= high) {
        count++;
        ++it;
    }
    return { (uint64_t)count, false };
}

TX OpResult op_st6_update_ap(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].x = (g_atomicParts[ap_idx].x + 1) % 100;
    g_atomicParts[ap_idx].y = (g_atomicParts[ap_idx].y + 1) % 100;
    return { 1, true };
}

TX OpResult op_st7_max_weight(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    TM_LOCAL int max_w = 0;
    for (TM_LOCAL int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
        if (ap_idx < (int)g_atomicParts.size())
            max_w = std::max(max_w, g_atomicParts[ap_idx].weight);
    }
    return { (uint64_t)max_w, false };
}

TX OpResult op_st8_update_ba(int ba_idx) {
    if (ba_idx < 0 || ba_idx >= (int)g_baseAssemblies.size()) return { 0, true };
    g_baseAssemblies[ba_idx].buildDate = (g_baseAssemblies[ba_idx].buildDate + 1) % 365 + 1000;
    return { 1, true };
}

TX OpResult op_st9_traverse_cp(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    TM_LOCAL int64_t sum = 0;
    for (TM_LOCAL int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
        if (ap_idx < (int)g_atomicParts.size())
            sum += g_atomicParts[ap_idx].weight;
    }
    return { (uint64_t)sum, false };
}

TX OpResult op_st10_update_doc(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, true };
    TM_LOCAL int doc_idx = g_compositeParts[cp_idx].documentId;
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return { 0, true };
    g_documents[doc_idx].buildDate = (g_documents[doc_idx].buildDate + 1) % 365 + 1000;
    return { 1, true };
}

// ─── SHORT OPERATIONS ────────────────────────────────────────────────

TX OpResult op_op1_lookup_ap(int id) {
    TM_LOCAL auto it = g_apById.find(id);
    if (it == g_apById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_atomicParts.size()) return { 0, false };
    TM_LOCAL auto &ap = g_atomicParts[it->second];
    return { (uint64_t)(ap.x + ap.y + ap.z), false };
}

TX OpResult op_op2_lookup_cp(int id) {
    TM_LOCAL auto it = g_cpById.find(id);
    if (it == g_cpById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_compositeParts.size()) return { 0, false };
    return { (uint64_t)g_compositeParts[it->second].buildDate, false };
}

TX OpResult op_op3_lookup_doc(int id) {
    TM_LOCAL auto it = g_docById.find(id);
    if (it == g_docById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_documents.size()) return { 0, false };
    return { (uint64_t)(g_documents[it->second].buildDate + g_documents[it->second].type), false };
}

TX OpResult op_op4_lookup_ba(int id) {
    TM_LOCAL auto it = g_baById.find(id);
    if (it == g_baById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_baseAssemblies.size()) return { 0, false };
    return { (uint64_t)g_baseAssemblies[it->second].buildDate, false };
}

TX OpResult op_op5_lookup_ca(int id) {
    TM_LOCAL auto it = g_caById.find(id);
    if (it == g_caById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_complexAssemblies.size()) return { 0, false };
    return { (uint64_t)g_complexAssemblies[it->second].buildDate, false };
}

TX OpResult op_op6_read_ap(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, false };
    TM_LOCAL auto &ap = g_atomicParts[ap_idx];
    return { (uint64_t)(ap.x + ap.y + ap.z), false };
}

TX OpResult op_op7_read_cp(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    return { (uint64_t)g_compositeParts[cp_idx].buildDate, false };
}

TX OpResult op_op8_count_docs(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    return { 1, false };
}

TX OpResult op_op9_sum_weights(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    TM_LOCAL int64_t sum = 0;
    for (TM_LOCAL int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
        if (ap_idx < (int)g_atomicParts.size())
            sum += g_atomicParts[ap_idx].weight;
    }
    return { (uint64_t)sum, false };
}

TX OpResult op_op10_count_conns(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, false };
    return { (uint64_t)g_atomicParts[ap_idx].connectionIds.size(), false };
}

TX OpResult op_op11_update_ap(int ap_idx, int nx, int ny) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].x = nx;
    g_atomicParts[ap_idx].y = ny;
    return { 1, true };
}

TX OpResult op_op12_update_weight(int ap_idx, int nw) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].weight = nw;
    return { 1, true };
}

TX OpResult op_op13_update_doc(int doc_idx, int nd) {
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return { 0, true };
    g_documents[doc_idx].buildDate = nd;
    return { 1, true };
}

TX OpResult op_op14_update_cp(int cp_idx, int nd) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, true };
    g_compositeParts[cp_idx].buildDate = nd;
    return { 1, true };
}

TX OpResult op_op15_update_ba(int ba_idx, int nd) {
    if (ba_idx < 0 || ba_idx >= (int)g_baseAssemblies.size()) return { 0, true };
    g_baseAssemblies[ba_idx].buildDate = nd;
    return { 1, true };
}

// ─── STRUCTURE MODIFICATIONS ─────────────────────────────────────────

TX OpResult op_sm1_create_cp(int new_id) {
    if ((int)g_compositeParts.size() >= MAX_CP * 2) return { 0, true };
    TM_LOCAL int cp_idx = (int)g_compositeParts.size();

    CompositePart cp;
    cp.id            = new_id;
    cp.buildDate     = 2000;
    cp.documentId    = (int)g_documents.size();
    cp.rootAtomicPartId = (int)g_atomicParts.size();
    g_compositeParts.push_back(cp);
    g_cpById[new_id] = cp_idx;
    g_cpByDate.insert({2000, cp_idx});

    Document doc;
    doc.id              = g_documents.size();
    doc.type            = new_id % 3;
    doc.buildDate       = 2000;
    doc.compositePartId = cp_idx;
    g_documents.push_back(doc);
    g_docById[doc.id] = (int)g_documents.size() - 1;

    TM_LOCAL int first_ap = cp.rootAtomicPartId;
    for (TM_LOCAL int j = 0; j < AP_PER_CP; j++) {
        AtomicPart ap;
        ap.id             = g_atomicParts.size();
        ap.x              = j % 100;
        ap.y              = (j / 100) % 100;
        ap.z              = j / 10000;
        ap.buildDate       = 2000;
        ap.weight          = 10;
        ap.compositePartId = cp_idx;
        g_atomicParts.push_back(ap);
        g_apById[ap.id] = (int)g_atomicParts.size() - 1;
        g_apByDate.insert({2000, (int)g_atomicParts.size() - 1});
        g_compositeParts[cp_idx].atomicPartIds.push_back(ap.id);
    }
    for (TM_LOCAL int j = 0; j < AP_PER_CP; j++) {
        TM_LOCAL int a = first_ap + j;
        TM_LOCAL int b = first_ap + (j + 1) % AP_PER_CP;
        Connection c;
        c.id               = g_connections.size();
        c.fromAtomicPartId = a;
        c.toAtomicPartId   = b;
        c.type             = j % 3;
        g_connections.push_back(c);
        g_atomicParts[a].connectionIds.push_back(c.id);
    }
    g_atomicPartCount = (int)g_atomicParts.size();
    g_connectionCount = (int)g_connections.size();
    return { 1, true };
}

TX OpResult op_sm2_delete_cp(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, true };
    g_compositeParts[cp_idx].id = -1;
    return { 1, true };
}

TX OpResult op_sm3_create_ap(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, true };
    if ((int)g_atomicParts.size() >= MAX_AP * 2) return { 0, true };
    AtomicPart ap;
    ap.id             = g_atomicParts.size();
    ap.x              = 0;  ap.y = 0;  ap.z = 0;
    ap.buildDate       = 2000;
    ap.weight          = 5;
    ap.compositePartId = cp_idx;
    g_atomicParts.push_back(ap);
    g_apById[ap.id] = (int)g_atomicParts.size() - 1;
    g_apByDate.insert({2000, (int)g_atomicParts.size() - 1});
    g_compositeParts[cp_idx].atomicPartIds.push_back(ap.id);
    g_atomicPartCount = (int)g_atomicParts.size();
    return { 1, true };
}

TX OpResult op_sm4_delete_ap(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].id = -1;
    return { 1, true };
}

TX OpResult op_sm5_create_conn(int from_ap, int to_ap, int typ) {
    if (from_ap < 0 || from_ap >= (int)g_atomicParts.size()) return { 0, true };
    if (to_ap < 0 || to_ap >= (int)g_atomicParts.size())   return { 0, true };
    Connection c;
    c.id               = g_connections.size();
    c.fromAtomicPartId = from_ap;
    c.toAtomicPartId   = to_ap;
    c.type             = typ;
    g_connections.push_back(c);
    g_atomicParts[from_ap].connectionIds.push_back(c.id);
    g_connectionCount = (int)g_connections.size();
    return { 1, true };
}

TX OpResult op_sm6_delete_conn(int conn_idx) {
    if (conn_idx < 0 || conn_idx >= (int)g_connections.size()) return { 0, true };
    TM_LOCAL int from_ap = g_connections[conn_idx].fromAtomicPartId;
    if (from_ap >= 0 && from_ap < (int)g_atomicParts.size()) {
        TM_LOCAL auto &clist = g_atomicParts[from_ap].connectionIds;
        clist.erase(std::remove(clist.begin(), clist.end(), conn_idx), clist.end());
    }
    g_connections[conn_idx].fromAtomicPartId = -1;
    return { 1, true };
}

TX OpResult op_sm7_create_ba(int parent_ca_idx) {
    if (parent_ca_idx < 0 || parent_ca_idx >= (int)g_complexAssemblies.size()) return { 0, true };
    TM_LOCAL auto &ca = g_complexAssemblies[parent_ca_idx];
    if (ca.level != TREE_LEVELS - 1) return { 0, true };
    TM_LOCAL int ba_idx = (int)g_baseAssemblies.size();
    BaseAssembly ba;
    ba.id                = ba_idx;
    ba.parentAssemblyId   = parent_ca_idx;
    ba.buildDate         = 2000;
    g_baseAssemblies.push_back(ba);
    ca.childBaseAssemblyIds.push_back(ba_idx);
    g_baById[ba_idx] = ba_idx;
    return { 1, true };
}

TX OpResult op_sm8_delete_ba(int ba_idx) {
    if (ba_idx < 0 || ba_idx >= (int)g_baseAssemblies.size()) return { 0, true };
    g_baseAssemblies[ba_idx].id = -1;
    TM_LOCAL int parent = g_baseAssemblies[ba_idx].parentAssemblyId;
    if (parent >= 0 && parent < (int)g_complexAssemblies.size()) {
        TM_LOCAL auto &clist = g_complexAssemblies[parent].childBaseAssemblyIds;
        clist.erase(std::remove(clist.begin(), clist.end(), ba_idx), clist.end());
    }
    return { 1, true };
}

// ─── OPERATION SELECTION ─────────────────────────────────────────────

enum class OpClass { LONG_TRAV, SHORT_TRAV, SHORT_OP, STRUCT_MOD };

struct OpDesc {
    OpClass cat;
    bool    isRead;
    int     id;
    OpResult (*func)(std::mt19937 &rng);
};

static OpResult wrap_st2(std::mt19937 &rng) {
    if (g_documents.empty()) return {0, false};
    return op_st2_traverse(pick_doc(rng));
}
static OpResult wrap_st3(std::mt19937 &rng) { return op_st3_traverse(pick_ap(rng)); }
static OpResult wrap_st4(std::mt19937 &rng) { return op_st4_update_ca(pick_ca(rng)); }
static OpResult wrap_st5(std::mt19937 &rng) {
    return op_st5_date_range(1000 + (rng() % 100), 1000 + 200 + (rng() % 100));
}
static OpResult wrap_st6(std::mt19937 &rng) { return op_st6_update_ap(pick_ap(rng)); }
static OpResult wrap_st7(std::mt19937 &rng) { return op_st7_max_weight(pick_cp(rng)); }
static OpResult wrap_st8(std::mt19937 &rng) { return op_st8_update_ba(pick_ba(rng)); }
static OpResult wrap_st9(std::mt19937 &rng) { return op_st9_traverse_cp(pick_cp(rng)); }
static OpResult wrap_st10(std::mt19937 &rng) { return op_st10_update_doc(pick_cp(rng)); }

static OpResult wrap_op1(std::mt19937 &rng) { return op_op1_lookup_ap(rng() % MAX_AP); }
static OpResult wrap_op2(std::mt19937 &rng) { return op_op2_lookup_cp(rng() % MAX_CP); }
static OpResult wrap_op3(std::mt19937 &rng) { return op_op3_lookup_doc(rng() % MAX_DOCUMENTS); }
static OpResult wrap_op4(std::mt19937 &rng) { return op_op4_lookup_ba(rng() % MAX_BA); }
static OpResult wrap_op5(std::mt19937 &rng) { return op_op5_lookup_ca(rng() % MAX_CA); }
static OpResult wrap_op6(std::mt19937 &rng) { return op_op6_read_ap(pick_ap(rng)); }
static OpResult wrap_op7(std::mt19937 &rng) { return op_op7_read_cp(pick_cp(rng)); }
static OpResult wrap_op8(std::mt19937 &rng) { return op_op8_count_docs(pick_cp(rng)); }
static OpResult wrap_op9(std::mt19937 &rng) { return op_op9_sum_weights(pick_cp(rng)); }
static OpResult wrap_op10(std::mt19937 &rng) { return op_op10_count_conns(pick_ap(rng)); }
static OpResult wrap_op11(std::mt19937 &rng) { return op_op11_update_ap(pick_ap(rng), rng() % 100, rng() % 100); }
static OpResult wrap_op12(std::mt19937 &rng) { return op_op12_update_weight(pick_ap(rng), (rng() % 50) + 1); }
static OpResult wrap_op13(std::mt19937 &rng) { return op_op13_update_doc(pick_doc(rng), 1000 + (rng() % 365)); }
static OpResult wrap_op14(std::mt19937 &rng) { return op_op14_update_cp(pick_cp(rng), 1000 + (rng() % 365)); }
static OpResult wrap_op15(std::mt19937 &rng) { return op_op15_update_ba(pick_ba(rng), 1000 + (rng() % 365)); }

static OpResult wrap_sm1(std::mt19937 &rng) { return op_sm1_create_cp(MAX_CP + (rng() % 1000)); }
static OpResult wrap_sm2(std::mt19937 &rng) { return op_sm2_delete_cp(pick_cp(rng)); }
static OpResult wrap_sm3(std::mt19937 &rng) { return op_sm3_create_ap(pick_cp(rng)); }
static OpResult wrap_sm4(std::mt19937 &rng) { return op_sm4_delete_ap(pick_ap(rng)); }
static OpResult wrap_sm5(std::mt19937 &rng) { return op_sm5_create_conn(pick_ap(rng), pick_ap(rng), rng() % 3); }
static OpResult wrap_sm6(std::mt19937 &rng) {
    TM_LOCAL int maxc = (int)g_connections.size();
    if (maxc == 0) return {0, true};
    return op_sm6_delete_conn(rng() % maxc);
}
static OpResult wrap_sm7(std::mt19937 &rng) {
    for (TM_LOCAL int i = 0; i < 100; i++) {
        TM_LOCAL int idx = pick_ca(rng);
        if (idx >= 0 && idx < (int)g_complexAssemblies.size() &&
            g_complexAssemblies[idx].level == TREE_LEVELS - 1)
            return op_sm7_create_ba(idx);
    }
    return {0, true};
}
static OpResult wrap_sm8(std::mt19937 &rng) { return op_sm8_delete_ba(pick_ba(rng)); }

// Operation descriptor tables
static OpDesc g_readOnlyLT[] = {
    {OpClass::LONG_TRAV, true, 0, [](auto &rng){ return op_lt1(); }},
    {OpClass::LONG_TRAV, true, 1, [](auto &rng){ return op_lt3(); }},
    {OpClass::LONG_TRAV, true, 2, [](auto &rng){ return op_lt5(); }},
};
static OpDesc g_updateLT[] = {
    {OpClass::LONG_TRAV, false, 0, [](auto &rng){ return op_lt2(); }},
    {OpClass::LONG_TRAV, false, 1, [](auto &rng){ return op_lt4(); }},
};
static OpDesc g_readOnlyST[] = {
    {OpClass::SHORT_TRAV, true, 0, wrap_st2},
    {OpClass::SHORT_TRAV, true, 1, wrap_st3},
    {OpClass::SHORT_TRAV, true, 2, wrap_st5},
    {OpClass::SHORT_TRAV, true, 3, wrap_st7},
    {OpClass::SHORT_TRAV, true, 4, wrap_st9},
};
static OpDesc g_updateST[] = {
    {OpClass::SHORT_TRAV, false, 0, wrap_st4},
    {OpClass::SHORT_TRAV, false, 1, wrap_st6},
    {OpClass::SHORT_TRAV, false, 2, wrap_st8},
    {OpClass::SHORT_TRAV, false, 3, wrap_st10},
    {OpClass::SHORT_TRAV, false, 4, [](auto &rng){ return op_st1(); }},
};
static OpDesc g_readOnlyOP[] = {
    {OpClass::SHORT_OP, true, 0, wrap_op1},
    {OpClass::SHORT_OP, true, 1, wrap_op2},
    {OpClass::SHORT_OP, true, 2, wrap_op3},
    {OpClass::SHORT_OP, true, 3, wrap_op4},
    {OpClass::SHORT_OP, true, 4, wrap_op5},
    {OpClass::SHORT_OP, true, 5, wrap_op6},
    {OpClass::SHORT_OP, true, 6, wrap_op7},
    {OpClass::SHORT_OP, true, 7, wrap_op8},
    {OpClass::SHORT_OP, true, 8, wrap_op9},
    {OpClass::SHORT_OP, true, 9, wrap_op10},
};
static OpDesc g_updateOP[] = {
    {OpClass::SHORT_OP, false, 0, wrap_op11},
    {OpClass::SHORT_OP, false, 1, wrap_op12},
    {OpClass::SHORT_OP, false, 2, wrap_op13},
    {OpClass::SHORT_OP, false, 3, wrap_op14},
    {OpClass::SHORT_OP, false, 4, wrap_op15},
};
static OpDesc g_structMod[] = {
    {OpClass::STRUCT_MOD, false, 0, wrap_sm1},
    {OpClass::STRUCT_MOD, false, 1, wrap_sm2},
    {OpClass::STRUCT_MOD, false, 2, wrap_sm3},
    {OpClass::STRUCT_MOD, false, 3, wrap_sm4},
    {OpClass::STRUCT_MOD, false, 4, wrap_sm5},
    {OpClass::STRUCT_MOD, false, 5, wrap_sm6},
    {OpClass::STRUCT_MOD, false, 6, wrap_sm7},
    {OpClass::STRUCT_MOD, false, 7, wrap_sm8},
};

constexpr int NUM_RO_LT = sizeof(g_readOnlyLT) / sizeof(g_readOnlyLT[0]);
constexpr int NUM_UP_LT = sizeof(g_updateLT) / sizeof(g_updateLT[0]);
constexpr int NUM_RO_ST = sizeof(g_readOnlyST) / sizeof(g_readOnlyST[0]);
constexpr int NUM_UP_ST = sizeof(g_updateST) / sizeof(g_updateST[0]);
constexpr int NUM_RO_OP = sizeof(g_readOnlyOP) / sizeof(g_readOnlyOP[0]);
constexpr int NUM_UP_OP = sizeof(g_updateOP) / sizeof(g_updateOP[0]);
constexpr int NUM_SM    = sizeof(g_structMod) / sizeof(g_structMod[0]);

static OpDesc pick_operation(std::mt19937 &rng, int writePercent) {
    TM_LOCAL int r = rng() % 100;
    TM_LOCAL OpClass cat;
    if (r < 5)       cat = OpClass::LONG_TRAV;
    else if (r < 45) cat = OpClass::SHORT_TRAV;
    else if (r < 90) cat = OpClass::SHORT_OP;
    else             cat = OpClass::STRUCT_MOD;

    TM_LOCAL bool wantRead = (rng() % 100) < (100 - writePercent);

    if (cat == OpClass::STRUCT_MOD) {
        return g_structMod[rng() % NUM_SM];
    }

    const OpDesc *pool;
    TM_LOCAL int poolSize;
    if (wantRead) {
        switch (cat) {
            case OpClass::LONG_TRAV:   pool = g_readOnlyLT; poolSize = NUM_RO_LT; break;
            case OpClass::SHORT_TRAV:  pool = g_readOnlyST; poolSize = NUM_RO_ST; break;
            case OpClass::SHORT_OP:    pool = g_readOnlyOP; poolSize = NUM_RO_OP; break;
            default: pool = g_readOnlyOP; poolSize = NUM_RO_OP;
        }
    } else {
        switch (cat) {
            case OpClass::LONG_TRAV:   pool = g_updateLT; poolSize = NUM_UP_LT; break;
            case OpClass::SHORT_TRAV:  pool = g_updateST; poolSize = NUM_UP_ST; break;
            case OpClass::SHORT_OP:    pool = g_updateOP; poolSize = NUM_UP_OP; break;
            default: pool = g_updateOP; poolSize = NUM_UP_OP;
        }
    }
    return pool[rng() % poolSize];
}

// ─── Worker ──────────────────────────────────────────────────────────

class Barrier {
    std::mutex mtx_;
    std::condition_variable cv_;
    int count_;
    int num_threads_;
    int crossing_;
public:
    explicit Barrier(int n) : count_(n), num_threads_(n), crossing_(0) {}
    void wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        crossing_++;
        if (crossing_ < num_threads_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

std::atomic<bool>   g_stop_workers{false};
std::atomic<uint64_t> g_total_ops{0};
std::atomic<uint64_t> g_lt_count{0};
std::atomic<uint64_t> g_st_count{0};
std::atomic<uint64_t> g_op_count{0};
std::atomic<uint64_t> g_sm_count{0};
std::atomic<uint64_t> g_ro_count{0};
std::atomic<uint64_t> g_up_count{0};

struct ThreadData {
    Barrier *barrier;
    int thread_id;
    int loops;
    int writePercent;
};

THREAD void worker(ThreadData *data) {
    TM_LOCAL std::mt19937 rng(data->thread_id * 12345 + 42 + 1);
    data->barrier->wait();

    TM_LOCAL int ops = 0;
    TM_LOCAL int lt = 0, st = 0, sop = 0, sm = 0, ro = 0, up = 0;

    while (!g_stop_workers.load(std::memory_order_relaxed) && ops < data->loops) {
        TM_LOCAL OpDesc desc = pick_operation(rng, data->writePercent);
        TM_LOCAL OpResult res = desc.func(rng);
        (void)res;
        ops++;
        switch (desc.cat) {
            case OpClass::LONG_TRAV:   lt++;  break;
            case OpClass::SHORT_TRAV:  st++;  break;
            case OpClass::SHORT_OP:    sop++; break;
            case OpClass::STRUCT_MOD:  sm++;  break;
        }
        if (desc.isRead) ro++; else up++;
    }

    g_total_ops.fetch_add(ops, std::memory_order_relaxed);
    g_lt_count.fetch_add(lt, std::memory_order_relaxed);
    g_st_count.fetch_add(st, std::memory_order_relaxed);
    g_op_count.fetch_add(sop, std::memory_order_relaxed);
    g_sm_count.fetch_add(sm, std::memory_order_relaxed);
    g_ro_count.fetch_add(ro, std::memory_order_relaxed);
    g_up_count.fetch_add(up, std::memory_order_relaxed);
}

// ─── Main ────────────────────────────────────────────────────────────

MAIN int main(int argc, char *argv[]) {
    TM_LOCAL int nb_threads   = DEFAULT_NB_THREADS;
    TM_LOCAL int duration_ms  = DEFAULT_DURATION_MS;
    TM_LOCAL int workload     = 1;
    TM_LOCAL int locking_mode = 0;

    for (TM_LOCAL int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) nb_threads = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration_ms = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) workload = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) locking_mode = std::atoi(argv[++i]);
    }

    TM_LOCAL int writePercent;
    switch (workload) {
        case 1: writePercent = 10; break;
        case 2: writePercent = 40; break;
        case 3: writePercent = 90; break;
        default: writePercent = 10;
    }

    std::cout << "STMbench7 (OPTIMIZED with TM_LOCAL)\n"
              << "====================================\n"
              << "Workload:   " << workload << " (" << (100 - writePercent) << "% read, " << writePercent << "% write)\n"
              << "Threads:    " << nb_threads << "\n"
              << "Duration:   " << duration_ms << " ms\n"
              << std::endl;

    init_data();

    std::cout << "Data structure:\n"
              << "  Modules:           " << g_modules.size() << "\n"
              << "  ComplexAssemblies: " << g_complexAssemblies.size() << "\n"
              << "  BaseAssemblies:    " << g_baseAssemblies.size() << "\n"
              << "  CompositeParts:    " << g_compositeParts.size() << "\n"
              << "  AtomicParts:       " << g_atomicParts.size() << "\n"
              << "  Connections:       " << g_connections.size() << "\n"
              << "  Documents:         " << g_documents.size() << "\n"
              << std::endl;

    TM_LOCAL int loops = duration_ms / 10;

    Barrier barrier(nb_threads);
    std::vector<ThreadData> thread_data(nb_threads);
    std::vector<std::thread> threads;

    for (TM_LOCAL int i = 0; i < nb_threads; i++) {
        thread_data[i].barrier       = &barrier;
        thread_data[i].thread_id     = i;
        thread_data[i].loops         = loops;
        thread_data[i].writePercent  = writePercent;
    }

    TM_LOCAL auto start_time = std::chrono::high_resolution_clock::now();

    for (TM_LOCAL int i = 0; i < nb_threads; i++)
        threads.emplace_back(worker, &thread_data[i]);

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    g_stop_workers.store(true, std::memory_order_release);
    for (auto &t : threads) t.join();

    TM_LOCAL auto end_time = std::chrono::high_resolution_clock::now();
    TM_LOCAL auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    TM_LOCAL uint64_t ops = g_total_ops.load();
    TM_LOCAL uint64_t lt  = g_lt_count.load();
    TM_LOCAL uint64_t st  = g_st_count.load();
    TM_LOCAL uint64_t sop = g_op_count.load();
    TM_LOCAL uint64_t sm  = g_sm_count.load();
    TM_LOCAL uint64_t ro  = g_ro_count.load();
    TM_LOCAL uint64_t up  = g_up_count.load();

    std::cout << "\nResults\n=======\n"
              << "Elapsed:       " << elapsed_ms << " ms\n"
              << "Total ops:     " << ops << "\n"
              << "Ops/sec:       " << (ops * 1000.0 / elapsed_ms) << "\n"
              << "\nCategory breakdown:\n"
              << "  LT: " << lt << " (" << (ops > 0 ? lt * 100.0 / ops : 0.0) << "%, spec ~5%)\n"
              << "  ST: " << st << " (" << (ops > 0 ? st * 100.0 / ops : 0.0) << "%, spec ~40%)\n"
              << "  OP: " << sop << " (" << (ops > 0 ? sop * 100.0 / ops : 0.0) << "%, spec ~45%)\n"
              << "  SM: " << sm << " (" << (ops > 0 ? sm * 100.0 / ops : 0.0) << "%, spec ~10%)\n"
              << "RO: " << ro << ", UP: " << up << "\n"
              << std::endl;

    return 0;
}
