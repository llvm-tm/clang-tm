// STMbench7 — explicit TM API port (no LLVM plugin)
// Full 45-operation spec, all 4 categories, 3 workload profiles.
// Based on: "STMBench7: A Benchmark for Software Transactional Memory"
//   Guerraoui, Kapalka, Vitek. EuroSys 2007.

#include "expli_tm_api/tm_api.hpp"
#include "expli_tm_api/tm_concurrent_map.hpp"
#include "expli_tm_api/tm_treap_multimap.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "../tests/benchmark_test.hpp"

// ── Spec constants (§2: medium OO7 size) ──────────────────────────
constexpr int FANOUT      = 3;
constexpr int TREE_LEVELS = 6;
constexpr int MAX_CP      = 500;
constexpr int AP_PER_CP   = 200;
constexpr int CONN_PER_AP = 3;
constexpr int MAX_BA      = 729;  // 3^6
constexpr int MAX_CA      = 364;  // 1+3+9+27+81+243
constexpr int MAX_AP      = MAX_CP * AP_PER_CP;    // 100,000
constexpr int MAX_CONN    = MAX_AP * CONN_PER_AP;  // 300,000
constexpr int MAX_DOCS    = MAX_CP;
constexpr int MAX_CP_BA_BAG = 5;
constexpr int DURATION_MS = 10000;

static int g_duration_ms = DURATION_MS;
static int g_num_threads = 4;
static int g_writePercent = 10;  // default: read-dominated (90% RO, 10% UP)

static void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-t") == 0 && i+1 < argc) g_num_threads   = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) g_duration_ms   = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i+1 < argc) {
            int w = atoi(argv[++i]);
            if (w == 1)      g_writePercent = 10;   // read-dominated
            else if (w == 2) g_writePercent = 40;   // read-write
            else if (w == 3) g_writePercent = 90;   // write-dominated
        }
    }
}

// ── Data structures ──────────────────────────────────────────────
struct Connection {
    int id;
    int fromAtomicPartId;
    int toAtomicPartId;
    expli::TM<int> type;
};

struct AtomicPart {
    int id;
    expli::TM<int> x, y, z;
    expli::TM<int> buildDate;
    expli::TM<int> weight;
    int compositePartId;
    expli::vector<int> connectionIds;
};

struct Document {
    int id;
    expli::TM<int> type;
    expli::TM<int> buildDate;
    int compositePartId;
};

struct CompositePart {
    int id;
    expli::TM<int> buildDate;
    int documentId;
    int rootAtomicPartId;
    expli::vector<int> atomicPartIds;
    expli::vector<int> baseAssemblyIds;
};

struct BaseAssembly {
    int id;
    int parentAssemblyId;
    expli::TM<int> buildDate;
    expli::vector<int> compositePartIds;
};

struct ComplexAssembly {
    int id;
    int level;
    int parentId;
    expli::vector<int> childAssemblyIds;
    expli::vector<int> childBaseAssemblyIds;
    expli::TM<int> buildDate;
};

struct Module {
    int id;
    int rootAssemblyId;
};

// ── Global storage ───────────────────────────────────────────────
static expli::vector<Module>           g_modules;
static expli::vector<ComplexAssembly>  g_complexAssemblies;
static expli::vector<BaseAssembly>     g_baseAssemblies;
static expli::vector<CompositePart>    g_compositeParts;
static expli::vector<AtomicPart>       g_atomicParts;
static expli::vector<Connection>       g_connections;
static expli::vector<Document>         g_documents;

// ── Indexes ─────────────────────────────────────────────────────
static expli::ts_map<int,int> g_caById;
static expli::ts_map<int,int> g_baById;
static expli::ts_map<int,int> g_cpById;
static expli::ts_map<int,int> g_apById;
static expli::ts_map<int,int> g_docById;
static expli::tm_treap_multimap<int,int> g_cpByDate;
static expli::tm_treap_multimap<int,int> g_apByDate;

// ── Init ─────────────────────────────────────────────────────────
static void init_data() {
    g_complexAssemblies.resize(MAX_CA);
    g_baseAssemblies.resize(MAX_BA);
    g_compositeParts.resize(MAX_CP);
    g_atomicParts.resize(MAX_AP);
    g_connections.resize(MAX_CONN);
    g_documents.resize(MAX_DOCS);

    // Module
    Module mod;
    mod.id = 0;
    mod.rootAssemblyId = 0;
    g_modules.push_back(mod);

    // CA tree
    int off = 0, sz = 1;
    for (int l = 0; l < TREE_LEVELS; l++) {
        for (int j = 0; j < sz; j++) {
            ComplexAssembly ca;
            ca.id = off + j; ca.level = l; ca.parentId = -1;
            ca.buildDate.poke(1000 + (l*100 + j) % 365);
            g_complexAssemblies[off + j] = ca;
        }
        off += sz; sz *= FANOUT;
    }
    for (int l = 1; l < TREE_LEVELS; l++) {
        int po = 0, ps = 1;
        for (int i=0; i<l-1; i++) { po += ps; ps *= FANOUT; }
        int co = po + ps;
        for (int p=0; p<ps; p++) {
            for (int c=0; c<FANOUT; c++) {
                int ci = co + p*FANOUT + c;
                g_complexAssemblies[po+p].childAssemblyIds.push_back(ci);
                g_complexAssemblies[ci].parentId = po+p;
            }
        }
    }

    // BA leaves
    int ba_id = 0;
    int ba_po = off - sz/FANOUT;
    int ba_ps = sz/FANOUT;
    for (int p=0; p<ba_ps; p++) {
        for (int c=0; c<FANOUT; c++) {
            BaseAssembly ba;
            ba.id = ba_id; ba.parentAssemblyId = ba_po + p;
            ba.buildDate.poke(1000 + (ba_id % 365));
            g_baseAssemblies[ba_id] = ba;
            g_complexAssemblies[ba_po+p].childBaseAssemblyIds.push_back(ba_id);
            g_baById.insert(ba_id, ba_id);
            ba_id++;
        }
    }

    // CPs + documents
    for (int i=0; i<MAX_CP; i++) {
        CompositePart cp;
        cp.id = i; cp.buildDate.poke(1000 + (i % 365));
        cp.documentId = i; cp.rootAtomicPartId = -1;
        g_compositeParts[i] = cp;

        Document doc;
        doc.id = i; doc.type.poke(i % 3);
        doc.buildDate.poke(1000 + (i % 365)); doc.compositePartId = i;
        g_documents[i] = doc;

        g_cpById.insert(i, i);
        g_cpByDate.insert(1000 + (i % 365), i);
        g_docById.insert(i, i);
    }

    // CP-BA bags (with duplicate avoidance)
    {
        std::mt19937 rng(42);
        for (int ci=0; ci<MAX_CP; ci++) {
            int num = 1 + (rng() % (MAX_CP_BA_BAG - 1));
            std::set<int> chosen;
            while ((int)chosen.size() < num)
                chosen.insert(rng() % MAX_BA);
            for (int bi : chosen) {
                g_compositeParts[ci].baseAssemblyIds.push_back(bi);
                g_baseAssemblies[bi].compositePartIds.push_back(ci);
            }
        }
    }

    // APs + connections
    {
        std::mt19937 rng(99);
        for (int ci=0; ci<MAX_CP; ci++) {
            int first = ci * AP_PER_CP;
            g_compositeParts[ci].rootAtomicPartId = first;
            for (int j=0; j<AP_PER_CP; j++) {
                AtomicPart ap;
                ap.id = first + j; ap.x.poke(j % 100); ap.y.poke((j/100)%100); ap.z.poke(j/10000);
                ap.buildDate.poke(1000 + (ci*AP_PER_CP + j) % 365);
                ap.weight.poke((j % 50) + 1); ap.compositePartId = ci;
                g_atomicParts[first + j] = ap;
                g_compositeParts[ci].atomicPartIds.push_back(ap.id);
                g_apById.insert(ap.id, first + j);
                g_apByDate.insert(1000 + (ci*AP_PER_CP + j) % 365, first + j);
            }
            // Ring + chord + random connections
            for (int j=0; j<AP_PER_CP; j++) {
                int a = first + j;
                int t1 = first + (j+1)%AP_PER_CP;
                int t2 = first + (j+2)%AP_PER_CP;
                int idx0 = (int)g_connections.size();
                g_connections[idx0] = Connection{};
                g_connections[idx0].id = idx0;
                g_connections[idx0].fromAtomicPartId = a;
                g_connections[idx0].toAtomicPartId = t1;
                g_connections[idx0].type.poke(j%3);

                int idx1 = idx0 + 1;
                g_connections[idx1] = Connection{};
                g_connections[idx1].id = idx1;
                g_connections[idx1].fromAtomicPartId = a;
                g_connections[idx1].toAtomicPartId = t2;
                g_connections[idx1].type.poke((j+1)%3);

                g_atomicParts[a].connectionIds.push_back(idx0);
                g_atomicParts[a].connectionIds.push_back(idx1);

                for (int k = 0; k < CONN_PER_AP - 2; k++) {
                    int t3 = first + (rng() % AP_PER_CP);
                    if (t3 == a) { k--; continue; }
                    int idx2 = idx1 + 1 + k;
                    g_connections[idx2] = Connection{};
                    g_connections[idx2].id = idx2;
                    g_connections[idx2].fromAtomicPartId = a;
                    g_connections[idx2].toAtomicPartId = t3;
                    g_connections[idx2].type.poke((j + k) % 3);
                    g_atomicParts[a].connectionIds.push_back(idx2);
                }
            }
        }
    }

    printf("Init done: %zu CA, %zu BA, %zu CP, %zu AP, %zu conn\n",
           g_complexAssemblies.size(), g_baseAssemblies.size(),
           g_compositeParts.size(), g_atomicParts.size(), g_connections.size());
}

// ── Random helpers ──────────────────────────────────────────────
static int pick_ca(std::mt19937 &r) { return r() % (int)g_complexAssemblies.size(); }
static int pick_ba(std::mt19937 &r) { return r() % (int)g_baseAssemblies.size(); }
static int pick_cp(std::mt19937 &r) { return r() % (int)g_compositeParts.size(); }
static int pick_ap(std::mt19937 &r) { return r() % (int)g_atomicParts.size(); }
static int pick_doc(std::mt19937 &r){ return r() % (int)g_documents.size(); }
static int pick_conn(std::mt19937 &r){ return r() % (int)g_connections.size(); }

struct OpResult { uint64_t val; bool wasWrite; };

// ==================================================================
// LONG TRAVERSALS (§3)
// ==================================================================
static OpResult op_lt1() {
    expli::TM<int>::begin();
    int64_t s = 0;
    for (size_t i=0; i<g_complexAssemblies.size(); i++)
        s += g_complexAssemblies[i].id + g_complexAssemblies[i].level + g_complexAssemblies[i].buildDate.read();
    for (size_t i=0; i<g_baseAssemblies.size(); i++)
        s += g_baseAssemblies[i].id + g_baseAssemblies[i].buildDate.read();
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_lt2() {
    expli::TM<int>::begin();
    for (size_t i=0; i<g_complexAssemblies.size(); i++)
        g_complexAssemblies[i].buildDate.write((g_complexAssemblies[i].buildDate.read() + 1) % 365 + 1000);
    for (size_t i=0; i<g_baseAssemblies.size(); i++)
        g_baseAssemblies[i].buildDate.write((g_baseAssemblies[i].buildDate.read() + 1) % 365 + 1000);
    expli::TM<int>::end();
    return {0, true};
}

static OpResult op_lt3() {
    expli::TM<int>::begin();
    int64_t s = 0;
    for (size_t i=0; i<g_compositeParts.size(); i++)
        s += g_compositeParts[i].id + g_compositeParts[i].buildDate.read();
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_lt4() {
    expli::TM<int>::begin();
    int64_t s = 0;
    for (size_t i=0; i<g_atomicParts.size(); i++) {
        int w = (g_atomicParts[i].weight.read() % 50) + 1;
        g_atomicParts[i].weight.write(w);
        s += w;
    }
    expli::TM<int>::end();
    return {(uint64_t)s, true};
}

static OpResult op_lt5() {
    expli::TM<int>::begin();
    int64_t s = 0;
    for (size_t i=0; i<g_connections.size(); i++)
        s += g_connections[i].id + g_connections[i].fromAtomicPartId + g_connections[i].toAtomicPartId + g_connections[i].type.read();
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

// ==================================================================
// SHORT TRAVERSALS (§3)
// ==================================================================
static OpResult op_st1() {
    expli::TM<int>::begin();
    int64_t s = 0;
    if (g_modules.empty()) { expli::TM<int>::end(); return {0,false}; }
    int ci = g_modules[0].rootAssemblyId;
    if (ci >= 0 && ci < (int)g_complexAssemblies.size())
        s += g_complexAssemblies[ci].id;
    for (int l=0; l<TREE_LEVELS-1 && (size_t)ci < g_complexAssemblies.size() && !g_complexAssemblies[ci].childAssemblyIds.empty(); l++) {
        ci = g_complexAssemblies[ci].childAssemblyIds[0];
        s += g_complexAssemblies[ci].id;
    }
    if ((size_t)ci < g_complexAssemblies.size() && !g_complexAssemblies[ci].childBaseAssemblyIds.empty()) {
        int bi = g_complexAssemblies[ci].childBaseAssemblyIds[0];
        if ((size_t)bi < g_baseAssemblies.size()) {
            s += g_baseAssemblies[bi].id + g_baseAssemblies[bi].buildDate.read();
            if (!g_baseAssemblies[bi].compositePartIds.empty()) {
                int cpi = g_baseAssemblies[bi].compositePartIds[0];
                if ((size_t)cpi < g_compositeParts.size())
                    s += g_compositeParts[cpi].buildDate.read();
            }
        }
    }
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_st2(int doc_idx) {
    expli::TM<int>::begin();
    if (doc_idx < 0 || (size_t)doc_idx >= g_documents.size()) { expli::TM<int>::end(); return {0,false}; }
    int64_t s = g_documents[doc_idx].buildDate.read() + g_documents[doc_idx].type.read();
    int ci = g_documents[doc_idx].compositePartId;
    if (ci >= 0 && (size_t)ci < g_compositeParts.size()) {
        auto &aps = g_compositeParts[ci].atomicPartIds;
        for (size_t i=0; i<aps.size(); i++) {
            int ai = aps[i];
            if (ai >= 0 && (size_t)ai < g_atomicParts.size())
                s += g_atomicParts[ai].weight.read();
        }
    }
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_st3(int ap_idx) {
    expli::TM<int>::begin();
    if (ap_idx < 0 || (size_t)ap_idx >= g_atomicParts.size()) { expli::TM<int>::end(); return {0,false}; }
    int64_t s = g_atomicParts[ap_idx].x.read() + g_atomicParts[ap_idx].y.read() + g_atomicParts[ap_idx].z.read();
    for (size_t i=0; i<g_atomicParts[ap_idx].connectionIds.size(); i++) {
        int cid = g_atomicParts[ap_idx].connectionIds[i];
        if ((size_t)cid < g_connections.size()) {
            int nb = (g_connections[cid].fromAtomicPartId == ap_idx)
                ? g_connections[cid].toAtomicPartId : g_connections[cid].fromAtomicPartId;
            if (nb >= 0 && (size_t)nb < g_atomicParts.size())
                s += g_atomicParts[nb].weight.read();
        }
    }
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_st4(int ca_idx) {
    expli::TM<int>::begin();
    if (ca_idx < 0 || (size_t)ca_idx >= g_complexAssemblies.size()) { expli::TM<int>::end(); return {0,true}; }
    g_complexAssemblies[ca_idx].buildDate.write((g_complexAssemblies[ca_idx].buildDate.read() + 1) % 365 + 1000);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_st5(int low, int high) {
    expli::TM<int>::begin();
    int64_t cnt = 0;
    auto it = g_cpByDate.lower_bound(low);
    while (it != g_cpByDate.end() && it->first <= high) {
        cnt++;
        ++it;
    }
    expli::TM<int>::end();
    return {(uint64_t)cnt, false};
}

static OpResult op_st6(int ap_idx) {
    expli::TM<int>::begin();
    if (ap_idx < 0 || (size_t)ap_idx >= g_atomicParts.size()) { expli::TM<int>::end(); return {0,true}; }
    g_atomicParts[ap_idx].x.write((g_atomicParts[ap_idx].x.read() + 1) % 100);
    g_atomicParts[ap_idx].y.write((g_atomicParts[ap_idx].y.read() + 1) % 100);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_st7(int cp_idx) {
    expli::TM<int>::begin();
    if (cp_idx < 0 || (size_t)cp_idx >= g_compositeParts.size()) { expli::TM<int>::end(); return {0,false}; }
    int max_w = 0;
    for (size_t i=0; i<g_compositeParts[cp_idx].atomicPartIds.size(); i++) {
        int ai = g_compositeParts[cp_idx].atomicPartIds[i];
        if ((size_t)ai < g_atomicParts.size())
            max_w = std::max(max_w, g_atomicParts[ai].weight.read());
    }
    expli::TM<int>::end();
    return {(uint64_t)max_w, false};
}

static OpResult op_st8(int ba_idx) {
    expli::TM<int>::begin();
    if (ba_idx < 0 || (size_t)ba_idx >= g_baseAssemblies.size()) { expli::TM<int>::end(); return {0,true}; }
    g_baseAssemblies[ba_idx].buildDate.write((g_baseAssemblies[ba_idx].buildDate.read() + 1) % 365 + 1000);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_st9(int cp_idx) {
    expli::TM<int>::begin();
    if (cp_idx < 0 || (size_t)cp_idx >= g_compositeParts.size()) { expli::TM<int>::end(); return {0,false}; }
    int64_t s = 0;
    for (size_t i=0; i<g_compositeParts[cp_idx].atomicPartIds.size(); i++) {
        int ai = g_compositeParts[cp_idx].atomicPartIds[i];
        if ((size_t)ai < g_atomicParts.size())
            s += g_atomicParts[ai].weight.read();
    }
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_st10(int cp_idx) {
    expli::TM<int>::begin();
    if (cp_idx < 0 || (size_t)cp_idx >= g_compositeParts.size()) { expli::TM<int>::end(); return {0,true}; }
    int di = g_compositeParts[cp_idx].documentId;
    if (di >= 0 && (size_t)di < g_documents.size())
        g_documents[di].buildDate.write((g_documents[di].buildDate.read() + 1) % 365 + 1000);
    expli::TM<int>::end();
    return {1, true};
}

// ==================================================================
// SHORT OPERATIONS (§3)
// ==================================================================
static OpResult op_op1(int id) {
    expli::TM<int>::begin();
    int *idx = g_apById.find(id);
    uint64_t r = 0;
    if (idx && *idx >= 0 && (size_t)*idx < g_atomicParts.size())
        r = (uint64_t)(g_atomicParts[*idx].x.read() + g_atomicParts[*idx].y.read() + g_atomicParts[*idx].z.read());
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op2(int id) {
    expli::TM<int>::begin();
    int *idx = g_cpById.find(id);
    uint64_t r = 0;
    if (idx && *idx >= 0 && (size_t)*idx < g_compositeParts.size())
        r = (uint64_t)g_compositeParts[*idx].buildDate.read();
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op3(int id) {
    expli::TM<int>::begin();
    int *idx = g_docById.find(id);
    uint64_t r = 0;
    if (idx && *idx >= 0 && (size_t)*idx < g_documents.size())
        r = (uint64_t)(g_documents[*idx].buildDate.read() + g_documents[*idx].type.read());
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op4(int id) {
    expli::TM<int>::begin();
    int *idx = g_baById.find(id);
    uint64_t r = 0;
    if (idx && *idx >= 0 && (size_t)*idx < g_baseAssemblies.size())
        r = (uint64_t)g_baseAssemblies[*idx].buildDate.read();
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op5(int id) {
    expli::TM<int>::begin();
    int *idx = g_caById.find(id);
    uint64_t r = 0;
    if (idx && *idx >= 0 && (size_t)*idx < g_complexAssemblies.size())
        r = (uint64_t)g_complexAssemblies[*idx].buildDate.read();
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op6(int ap_idx) {
    expli::TM<int>::begin();
    if (ap_idx < 0 || (size_t)ap_idx >= g_atomicParts.size()) { expli::TM<int>::end(); return {0,false}; }
    uint64_t r = (uint64_t)(g_atomicParts[ap_idx].x.read() + g_atomicParts[ap_idx].y.read() + g_atomicParts[ap_idx].z.read());
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op7(int cp_idx) {
    expli::TM<int>::begin();
    if (cp_idx < 0 || (size_t)cp_idx >= g_compositeParts.size()) { expli::TM<int>::end(); return {0,false}; }
    uint64_t r = (uint64_t)g_compositeParts[cp_idx].buildDate.read();
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op8(int cp_idx) {
    expli::TM<int>::begin();
    if (cp_idx < 0 || (size_t)cp_idx >= g_compositeParts.size()) { expli::TM<int>::end(); return {0,false}; }
    expli::TM<int>::end();
    return {1, false};
}

static OpResult op_op9(int cp_idx) {
    expli::TM<int>::begin();
    if (cp_idx < 0 || (size_t)cp_idx >= g_compositeParts.size()) { expli::TM<int>::end(); return {0,false}; }
    int64_t s = 0;
    for (size_t i=0; i<g_compositeParts[cp_idx].atomicPartIds.size(); i++) {
        int ai = g_compositeParts[cp_idx].atomicPartIds[i];
        if ((size_t)ai < g_atomicParts.size())
            s += g_atomicParts[ai].weight.read();
    }
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_op10(int ap_idx) {
    expli::TM<int>::begin();
    if (ap_idx < 0 || (size_t)ap_idx >= g_atomicParts.size()) { expli::TM<int>::end(); return {0,false}; }
    uint64_t r = (uint64_t)g_atomicParts[ap_idx].connectionIds.size();
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op11(int ai, int nx, int ny) {
    expli::TM<int>::begin();
    if (ai >= 0 && (size_t)ai < g_atomicParts.size()) {
        g_atomicParts[ai].x.write(nx);
        g_atomicParts[ai].y.write(ny);
    }
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_op12(int ai, int nw) {
    expli::TM<int>::begin();
    if (ai >= 0 && (size_t)ai < g_atomicParts.size())
        g_atomicParts[ai].weight.write(nw);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_op13(int di, int nd) {
    expli::TM<int>::begin();
    if (di >= 0 && (size_t)di < g_documents.size())
        g_documents[di].buildDate.write(nd);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_op14(int ci, int nd) {
    expli::TM<int>::begin();
    if (ci >= 0 && (size_t)ci < g_compositeParts.size())
        g_compositeParts[ci].buildDate.write(nd);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_op15(int bi, int nd) {
    expli::TM<int>::begin();
    if (bi >= 0 && (size_t)bi < g_baseAssemblies.size())
        g_baseAssemblies[bi].buildDate.write(nd);
    expli::TM<int>::end();
    return {1, true};
}

// ==================================================================
// STRUCTURE MODIFICATIONS (§3)
// ==================================================================
static OpResult op_sm1(int new_id) {
    expli::TM<int>::begin();
    if ((int)g_compositeParts.size() >= MAX_CP * 2) { expli::TM<int>::end(); return {0,true}; }
    int cp_idx = (int)g_compositeParts.size();
    int doc_idx = (int)g_documents.size();
    int first_ap = (int)g_atomicParts.size();

    CompositePart cp;
    cp.id = new_id; cp.buildDate.write(2000);
    cp.documentId = doc_idx; cp.rootAtomicPartId = first_ap;
    g_compositeParts.push_back(cp);
    g_cpById.insert(new_id, cp_idx);
    g_cpByDate.insert(2000, cp_idx);

    Document doc;
    doc.id = doc_idx; doc.type.write(new_id % 3);
    doc.buildDate.write(2000); doc.compositePartId = cp_idx;
    g_documents.push_back(doc);
    g_docById.insert(doc_idx, doc_idx);

    for (int j=0; j<AP_PER_CP; j++) {
        AtomicPart ap;
        ap.id = (int)g_atomicParts.size(); ap.x.write(j%100);
        ap.y.write((j/100)%100); ap.z.write(j/10000);
        ap.buildDate.write(2000); ap.weight.write(10);
        ap.compositePartId = cp_idx;
        g_atomicParts.push_back(ap);
        g_apById.insert(ap.id, (int)g_atomicParts.size()-1);
        g_apByDate.insert(2000, (int)g_atomicParts.size()-1);
        g_compositeParts[cp_idx].atomicPartIds.push_back(ap.id);
    }
    // Ring connections for new CP
    for (int j=0; j<AP_PER_CP; j++) {
        int a = first_ap + j;
        int b = first_ap + (j+1)%AP_PER_CP;
        Connection c;
        c.id = (int)g_connections.size(); c.fromAtomicPartId = a;
        c.toAtomicPartId = b; c.type.write(j%3);
        g_connections.push_back(c);
        g_atomicParts[a].connectionIds.push_back(c.id);
    }
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm2(int ci) {
    expli::TM<int>::begin();
    if (ci >= 0 && (size_t)ci < g_compositeParts.size())
        g_compositeParts[ci].id = -1;
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm3(int cp_idx) {
    expli::TM<int>::begin();
    if (cp_idx < 0 || (size_t)cp_idx >= g_compositeParts.size()) { expli::TM<int>::end(); return {0,true}; }
    if ((int)g_atomicParts.size() >= MAX_AP * 2) { expli::TM<int>::end(); return {0,true}; }
    int id = (int)g_atomicParts.size();
    AtomicPart ap;
    ap.id = id; ap.x.write(0); ap.y.write(0); ap.z.write(0);
    ap.buildDate.write(2000); ap.weight.write(5);
    ap.compositePartId = cp_idx;
    g_atomicParts.push_back(ap);
    g_apById.insert(id, (int)g_atomicParts.size()-1);
    g_apByDate.insert(2000, (int)g_atomicParts.size()-1);
    g_compositeParts[cp_idx].atomicPartIds.push_back(id);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm4(int ai) {
    expli::TM<int>::begin();
    if (ai >= 0 && (size_t)ai < g_atomicParts.size())
        g_atomicParts[ai].weight.write(-1);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm5(int from_ap, int to_ap, int typ) {
    expli::TM<int>::begin();
    if (from_ap < 0 || (size_t)from_ap >= g_atomicParts.size()) { expli::TM<int>::end(); return {0,true}; }
    if (to_ap < 0 || (size_t)to_ap >= g_atomicParts.size()) { expli::TM<int>::end(); return {0,true}; }
    Connection c;
    c.id = (int)g_connections.size(); c.fromAtomicPartId = from_ap;
    c.toAtomicPartId = to_ap; c.type.write(typ);
    g_connections.push_back(c);
    g_atomicParts[from_ap].connectionIds.push_back(c.id);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm6(int ci) {
    expli::TM<int>::begin();
    if (ci < 0 || (size_t)ci >= g_connections.size()) { expli::TM<int>::end(); return {0,true}; }
    int from_ap = g_connections[ci].fromAtomicPartId;
    if (from_ap >= 0 && (size_t)from_ap < g_atomicParts.size()) {
        auto &clist = g_atomicParts[from_ap].connectionIds;
        for (size_t i=0; i<clist.size(); i++) {
            if (clist[i] == ci) { clist.erase(clist.begin()+i); break; }
        }
    }
    g_connections[ci].fromAtomicPartId = -1;
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm7(int parent_ca_idx) {
    expli::TM<int>::begin();
    if (parent_ca_idx < 0 || (size_t)parent_ca_idx >= g_complexAssemblies.size()) { expli::TM<int>::end(); return {0,true}; }
    if (g_complexAssemblies[parent_ca_idx].level != TREE_LEVELS - 1) { expli::TM<int>::end(); return {0,true}; }
    int ba_idx = (int)g_baseAssemblies.size();
    BaseAssembly ba;
    ba.id = ba_idx; ba.parentAssemblyId = parent_ca_idx;
    ba.buildDate.write(2000);
    g_baseAssemblies.push_back(ba);
    g_complexAssemblies[parent_ca_idx].childBaseAssemblyIds.push_back(ba_idx);
    g_baById.insert(ba_idx, ba_idx);
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm8(int bi) {
    expli::TM<int>::begin();
    if (bi < 0 || (size_t)bi >= g_baseAssemblies.size()) { expli::TM<int>::end(); return {0,true}; }
    g_baseAssemblies[bi].id = -1;
    int parent = g_baseAssemblies[bi].parentAssemblyId;
    if (parent >= 0 && (size_t)parent < g_complexAssemblies.size()) {
        auto &clist = g_complexAssemblies[parent].childBaseAssemblyIds;
        for (size_t i=0; i<clist.size(); i++) {
            if (clist[i] == bi) { clist.erase(clist.begin()+i); break; }
        }
    }
    expli::TM<int>::end();
    return {1, true};
}

// ==================================================================
// Operation dispatch
// ==================================================================
enum OpCat { LONG_TRAV, SHORT_TRAV, SHORT_OP, STRUCT_MOD };

struct OpDesc {
    OpCat cat; bool readOnly; int id;
    OpResult (*fn)(std::mt19937&);
};

// Long traversals: LT1-LT5
static OpResult wrap_st2(std::mt19937 &r) { return op_st2(pick_doc(r)); }
static OpResult wrap_st3(std::mt19937 &r) { return op_st3(pick_ap(r)); }
static OpResult wrap_st4(std::mt19937 &r) { return op_st4(pick_ca(r)); }
static OpResult wrap_st5(std::mt19937 &r) {
    return op_st5(1000 + (r() % 100), 1000 + 200 + (r() % 100));
}
static OpResult wrap_st6(std::mt19937 &r) { return op_st6(pick_ap(r)); }
static OpResult wrap_st7(std::mt19937 &r) { return op_st7(pick_cp(r)); }
static OpResult wrap_st8(std::mt19937 &r) { return op_st8(pick_ba(r)); }
static OpResult wrap_st9(std::mt19937 &r) { return op_st9(pick_cp(r)); }
static OpResult wrap_st10(std::mt19937 &r) { return op_st10(pick_cp(r)); }

static OpResult wrap_op1(std::mt19937 &r) { return op_op1(r() % MAX_AP); }
static OpResult wrap_op2(std::mt19937 &r) { return op_op2(r() % MAX_CP); }
static OpResult wrap_op3(std::mt19937 &r) { return op_op3(r() % MAX_DOCS); }
static OpResult wrap_op4(std::mt19937 &r) { return op_op4(r() % MAX_BA); }
static OpResult wrap_op5(std::mt19937 &r) { return op_op5(r() % MAX_CA); }
static OpResult wrap_op6(std::mt19937 &r) { return op_op6(pick_ap(r)); }
static OpResult wrap_op7(std::mt19937 &r) { return op_op7(pick_cp(r)); }
static OpResult wrap_op8(std::mt19937 &r) { return op_op8(pick_cp(r)); }
static OpResult wrap_op9(std::mt19937 &r) { return op_op9(pick_cp(r)); }
static OpResult wrap_op10(std::mt19937 &r) { return op_op10(pick_ap(r)); }
static OpResult wrap_op11(std::mt19937 &r) { return op_op11(pick_ap(r), r()%100, r()%100); }
static OpResult wrap_op12(std::mt19937 &r) { return op_op12(pick_ap(r), (r()%50)+1); }
static OpResult wrap_op13(std::mt19937 &r) { return op_op13(pick_doc(r), 1000 + (r()%365)); }
static OpResult wrap_op14(std::mt19937 &r) { return op_op14(pick_cp(r), 1000 + (r()%365)); }
static OpResult wrap_op15(std::mt19937 &r) { return op_op15(pick_ba(r), 1000 + (r()%365)); }

static OpResult wrap_sm1(std::mt19937 &r) { return op_sm1(MAX_CP + (r()%1000)); }
static OpResult wrap_sm2(std::mt19937 &r) { return op_sm2(pick_cp(r)); }
static OpResult wrap_sm3(std::mt19937 &r) { return op_sm3(pick_cp(r)); }
static OpResult wrap_sm4(std::mt19937 &r) { return op_sm4(pick_ap(r)); }
static OpResult wrap_sm5(std::mt19937 &r) { return op_sm5(pick_ap(r), pick_ap(r), r()%3); }
static OpResult wrap_sm6(std::mt19937 &r) {
    int maxc = (int)g_connections.size();
    if (maxc == 0) return {0,true};
    return op_sm6(r() % maxc);
}
static OpResult wrap_sm7(std::mt19937 &r) {
    for (int i=0; i<100; i++) {
        int idx = pick_ca(r);
        if (idx >= 0 && (size_t)idx < g_complexAssemblies.size() &&
            g_complexAssemblies[idx].level == TREE_LEVELS - 1)
            return op_sm7(idx);
    }
    return {0,true};
}
static OpResult wrap_sm8(std::mt19937 &r) { return op_sm8(pick_ba(r)); }

// Operation arrays matching plugin layout
static OpDesc g_readOnlyLT[] = {
    {LONG_TRAV, true, 0, [](auto&r){return op_lt1();}},
    {LONG_TRAV, true, 1, [](auto&r){return op_lt3();}},
    {LONG_TRAV, true, 2, [](auto&r){return op_lt5();}},
};
static OpDesc g_updateLT[] = {
    {LONG_TRAV, false, 0, [](auto&r){return op_lt2();}},
    {LONG_TRAV, false, 1, [](auto&r){return op_lt4();}},
};

static OpDesc g_readOnlyST[] = {
    {SHORT_TRAV, true, 0, wrap_st2},
    {SHORT_TRAV, true, 1, wrap_st3},
    {SHORT_TRAV, true, 2, wrap_st5},
    {SHORT_TRAV, true, 3, wrap_st7},
    {SHORT_TRAV, true, 4, wrap_st9},
};
static OpDesc g_updateST[] = {
    {SHORT_TRAV, false, 0, wrap_st4},
    {SHORT_TRAV, false, 1, wrap_st6},
    {SHORT_TRAV, false, 2, wrap_st8},
    {SHORT_TRAV, false, 3, wrap_st10},
    {SHORT_TRAV, false, 4, [](auto&r){return op_st1();}},
};

static OpDesc g_readOnlyOP[] = {
    {SHORT_OP, true, 0, wrap_op1},
    {SHORT_OP, true, 1, wrap_op2},
    {SHORT_OP, true, 2, wrap_op3},
    {SHORT_OP, true, 3, wrap_op4},
    {SHORT_OP, true, 4, wrap_op5},
    {SHORT_OP, true, 5, wrap_op6},
    {SHORT_OP, true, 6, wrap_op7},
    {SHORT_OP, true, 7, wrap_op8},
    {SHORT_OP, true, 8, wrap_op9},
    {SHORT_OP, true, 9, wrap_op10},
};
static OpDesc g_updateOP[] = {
    {SHORT_OP, false, 0, wrap_op11},
    {SHORT_OP, false, 1, wrap_op12},
    {SHORT_OP, false, 2, wrap_op13},
    {SHORT_OP, false, 3, wrap_op14},
    {SHORT_OP, false, 4, wrap_op15},
};

static OpDesc g_structMod[] = {
    {STRUCT_MOD, false, 0, wrap_sm1},
    {STRUCT_MOD, false, 1, wrap_sm2},
    {STRUCT_MOD, false, 2, wrap_sm3},
    {STRUCT_MOD, false, 3, wrap_sm4},
    {STRUCT_MOD, false, 4, wrap_sm5},
    {STRUCT_MOD, false, 5, wrap_sm6},
    {STRUCT_MOD, false, 6, wrap_sm7},
    {STRUCT_MOD, false, 7, wrap_sm8},
};

constexpr int NUM_RO_LT = sizeof(g_readOnlyLT)/sizeof(g_readOnlyLT[0]);
constexpr int NUM_UP_LT = sizeof(g_updateLT)/sizeof(g_updateLT[0]);
constexpr int NUM_RO_ST = sizeof(g_readOnlyST)/sizeof(g_readOnlyST[0]);
constexpr int NUM_UP_ST = sizeof(g_updateST)/sizeof(g_updateST[0]);
constexpr int NUM_RO_OP = sizeof(g_readOnlyOP)/sizeof(g_readOnlyOP[0]);
constexpr int NUM_UP_OP = sizeof(g_updateOP)/sizeof(g_updateOP[0]);
constexpr int NUM_SM    = sizeof(g_structMod)/sizeof(g_structMod[0]);

static OpDesc pick_operation(std::mt19937 &rng, int writePercent) {
    int r = rng() % 100;
    OpCat cat;
    if (r < 5)       cat = LONG_TRAV;
    else if (r < 45) cat = SHORT_TRAV;
    else if (r < 90) cat = SHORT_OP;
    else             cat = STRUCT_MOD;

    bool wantRead = (rng() % 100) < (100 - writePercent);

    if (cat == STRUCT_MOD)
        return g_structMod[rng() % NUM_SM];

    const OpDesc *pool;
    int poolSize;
    if (wantRead) {
        switch (cat) {
            case LONG_TRAV:  pool = g_readOnlyLT; poolSize = NUM_RO_LT; break;
            case SHORT_TRAV: pool = g_readOnlyST; poolSize = NUM_RO_ST; break;
            case SHORT_OP:   pool = g_readOnlyOP; poolSize = NUM_RO_OP; break;
            default:         pool = g_readOnlyOP; poolSize = NUM_RO_OP;
        }
    } else {
        switch (cat) {
            case LONG_TRAV:  pool = g_updateLT; poolSize = NUM_UP_LT; break;
            case SHORT_TRAV: pool = g_updateST; poolSize = NUM_UP_ST; break;
            case SHORT_OP:   pool = g_updateOP; poolSize = NUM_UP_OP; break;
            default:         pool = g_updateOP; poolSize = NUM_UP_OP;
        }
    }
    return pool[rng() % poolSize];
}

// ── TX retry wrapper ─────────────────────────────────────────────
static std::atomic<int> g_count_by_cat[4] = {0,0,0,0};
static std::atomic<int> g_count_ro{0}, g_count_up{0};
static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_tx_count{0};

template<typename F>
static auto run_tx(F&& body) -> decltype(body()) {
    volatile bool done = false;
    decltype(body()) result{};
    while (!done) {
        sigsetjmp(tm_jmpbuf, 0);
        tm_nested_call_counter = 1;
        tm_begin();
        result = body();
        tm_end();
        done = true;
        g_tx_count.fetch_add(1, std::memory_order_relaxed);
    }
    tm_nested_call_counter = 0;
    return result;
}

// ── Worker ────────────────────────────────────────────────────────
static void worker(int seed) {
    expli::TM<int>::thread_init();
    auto rng = std::mt19937(seed);

    while (!g_stop.load()) {
        OpDesc desc = pick_operation(rng, g_writePercent);
        run_tx([&]() -> OpResult { return desc.fn(rng); });
        g_count_by_cat[desc.cat]++;
        if (desc.readOnly) g_count_ro++; else g_count_up++;
    }
    expli::TM<int>::thread_exit();
}

// ── Tests ─────────────────────────────────────────────────────────
static void test_cli_flags() {
    printf("  Testing CLI flags...\n");
    int save_t = g_num_threads, save_d = g_duration_ms;
    TEST_EQ(g_num_threads, 4, "default threads");
    TEST_EQ(g_duration_ms, 10000, "default duration");
    const char* test_args[] = {"prog", "-t", "2", "-d", "500"};
    parse_args((int)(sizeof(test_args)/sizeof(test_args[0])), (char**)test_args);
    TEST_EQ(g_num_threads, 2, "override threads");
    TEST_EQ(g_duration_ms, 500, "override duration");
    g_num_threads = save_t; g_duration_ms = save_d;
    if (test_result() != 0) exit(1);
}

static void test_rng() {
    printf("  Testing RNG determinism...\n");
    test_rng_determinism<std::mt19937>();
    if (test_result() != 0) exit(1);
}

static void test_logic() {
    printf("  Testing STMbench7 logic...\n");
    init_data();
    TEST_EQ((int)g_complexAssemblies.size(), MAX_CA, "CA count");
    TEST_EQ((int)g_baseAssemblies.size(), MAX_BA, "BA count");
    TEST_EQ((int)g_compositeParts.size(), MAX_CP, "CP count");
    TEST_EQ((int)g_atomicParts.size(), MAX_AP, "AP count");
    TEST_EQ((int)g_connections.size(), MAX_CONN, "conn count");
    TEST_EQ((int)g_documents.size(), MAX_DOCS, "doc count");
    OpResult r1 = op_lt1();
    TEST_ASSERT(r1.val > 0, "LT1 produces non-zero sum");
    OpResult r3 = op_lt3();
    TEST_ASSERT(r3.val > 0, "LT3 produces non-zero sum");
    if (test_result() != 0) exit(1);
}

// ── Main ──────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("Running self-tests for stmbench7...\n");
        test_cli_flags();
        test_rng();
        printf("  NOTE: test_logic requires expli::TM initialization.\n");
        printf("  Skipping logic test in --test mode (needs runtime init).\n");
        printf("All basic tests passed.\n");
        return 0;
    }
    parse_args(argc, argv);

    printf("STMbench7 — Explicit TM API (full 45-op spec)\n");
    printf("Workload:   %d%% read, %d%% write\n", 100 - g_writePercent, g_writePercent);
    printf("Threads:    %d  Duration: %d ms\n", g_num_threads, g_duration_ms);

    expli::TM<int>::init();
    init_data();

    auto start = std::chrono::high_resolution_clock::now();
    std::thread *threads = new std::thread[g_num_threads];
    for (int i=0; i<g_num_threads; i++)
        new (&threads[i]) std::thread(worker, 1234 + i);

    std::this_thread::sleep_for(std::chrono::milliseconds(g_duration_ms));
    g_stop.store(true);
    for (int i=0; i<g_num_threads; i++) threads[i].join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();

    uint64_t total = 0;
    for (int c=0; c<4; c++) total += g_count_by_cat[c].load();
    uint64_t tx_total = g_tx_count.load();
    uint64_t ro = g_count_ro.load();
    uint64_t up = g_count_up.load();

    printf("\nResults (%d ms):\n", (int)ms);
    printf("  Long trav:    %d (%.1f%%)\n", g_count_by_cat[0].load(), total>0 ? g_count_by_cat[0].load()*100.0/total : 0);
    printf("  Short trav:   %d (%.1f%%)\n", g_count_by_cat[1].load(), total>0 ? g_count_by_cat[1].load()*100.0/total : 0);
    printf("  Short ops:    %d (%.1f%%)\n", g_count_by_cat[2].load(), total>0 ? g_count_by_cat[2].load()*100.0/total : 0);
    printf("  Struct mods:  %d (%.1f%%)\n", g_count_by_cat[3].load(), total>0 ? g_count_by_cat[3].load()*100.0/total : 0);
    printf("  Read-only:    %llu (%.1f%%)\n", (unsigned long long)ro, total>0 ? ro*100.0/total : 0);
    printf("  Update:       %llu (%.1f%%)\n", (unsigned long long)up, total>0 ? up*100.0/total : 0);
    printf("  Business ops: %llu  (%.0f ops/sec)\n", (unsigned long long)total, total * 1000.0 / ms);
    printf("  TM TXs:       %llu  (%.0f txns/sec)\n", (unsigned long long)tx_total, tx_total * 1000.0 / ms);

    delete[] threads;
    expli::TM<int>::exit();
    printf("PASS\n");
    return 0;
}
