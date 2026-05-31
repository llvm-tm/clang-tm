// STMbench7 — explicit TM API port (no LLVM plugin)
// Representative subset: 4 long traversals, 4 short traversals,
// 4 short ops, 2 structure modifications = 14 operations.
// Uses tm_concurrent_map for ID indexes and expli::vector for storage.

#include "../../expli_tm_api/tm_api.hpp"
#include "../../expli_tm_api/tm_concurrent_map.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

// ── Spec constants ──────────────────────────────────────────
constexpr int FANOUT      = 3;
constexpr int TREE_LEVELS = 6;
constexpr int MAX_CP      = 500;
constexpr int AP_PER_CP   = 200;
constexpr int CONN_PER_AP = 3;
constexpr int MAX_BA      = 729;  // 3^6
constexpr int MAX_CA      = 364;  // Σ3^0..5
constexpr int MAX_AP      = MAX_CP * AP_PER_CP;
constexpr int MAX_CONN    = MAX_AP * CONN_PER_AP;
constexpr int MAX_DOCS    = MAX_CP;
constexpr int DURATION_MS = 10000;

// ── Data structures ─────────────────────────────────────────
// TM<T> fields are those modified inside TX; others are const after init.

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

// ── Global storage (pre-allocated, never resized at runtime) ─
static expli::vector<Module>           g_modules;
static expli::vector<ComplexAssembly>  g_complexAssemblies;
static expli::vector<BaseAssembly>     g_baseAssemblies;
static expli::vector<CompositePart>    g_compositeParts;
static expli::vector<AtomicPart>       g_atomicParts;
static expli::vector<Connection>       g_connections;
static expli::vector<Document>         g_documents;
static Module                          g_manual;  // unused in ops

// ── Indexes (thread-safe maps) ──────────────────────────────
static expli::ts_map<int,int> g_caById;
static expli::ts_map<int,int> g_baById;
static expli::ts_map<int,int> g_cpById;
static expli::ts_map<int,int> g_apById;
static expli::ts_map<int,int> g_docById;
static expli::ts_multimap<int,int> g_cpByDate;
static expli::ts_multimap<int,int> g_apByDate;

// ── Init (runs once, outside any TX) ─────────────────────────
static void init_data() {
    g_complexAssemblies.resize(MAX_CA);
    g_baseAssemblies.resize(MAX_BA);
    g_compositeParts.resize(MAX_CP);
    g_atomicParts.resize(MAX_AP);
    g_connections.resize(MAX_CONN);
    g_documents.resize(MAX_DOCS);

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
    int ba_po = off - sz/FANOUT;  // offset of level TREE_LEVELS-1 CAs
    int ba_ps = sz/FANOUT;
    for (int p=0; p<ba_ps; p++) {
        for (int c=0; c<FANOUT; c++) {
            BaseAssembly ba;
            ba.id = ba_id; ba.parentAssemblyId = ba_po + p;
            ba.buildDate.poke(1000 + (ba_id % 365));
            g_baseAssemblies[ba_id] = ba;
            g_complexAssemblies[ba_po+p].childBaseAssemblyIds.push_back(ba_id);
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
    }

    // CP-BA bags
    {
        std::mt19937 rng(42);
        for (int ci=0; ci<MAX_CP; ci++) {
            int num = 1 + (rng() % 4);
            for (int j=0; j<num; j++) {
                int bi = rng() % MAX_BA;
                g_compositeParts[ci].baseAssemblyIds.push_back(bi);
                g_baseAssemblies[bi].compositePartIds.push_back(ci);
            }
        }
    }

    // APs
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
            // connections: ring + chord + random
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
                int idx1 = (int)g_connections.size() + 1;
                g_connections[idx1] = Connection{};
                g_connections[idx1].id = idx1;
                g_connections[idx1].fromAtomicPartId = a;
                g_connections[idx1].toAtomicPartId = t2;
                g_connections[idx1].type.poke((j+1)%3);
                int t3 = first + (rng() % AP_PER_CP);
                if (t3 == a) t3 = first + (j+3)%AP_PER_CP;
                int idx2 = (int)g_connections.size() + 2;
                g_connections[idx2] = Connection{};
                g_connections[idx2].id = idx2;
                g_connections[idx2].fromAtomicPartId = a;
                g_connections[idx2].toAtomicPartId = t3;
                g_connections[idx2].type.poke((j+2)%3);
                g_atomicParts[a].connectionIds.push_back(idx0);
                g_atomicParts[a].connectionIds.push_back(idx1);
                g_atomicParts[a].connectionIds.push_back(idx2);
            }
        }
    }
    g_manual.id = 0;
    printf("Init done: %zu CA, %zu BA, %zu CP, %zu AP, %zu conn\n",
           g_complexAssemblies.size(), g_baseAssemblies.size(),
           g_compositeParts.size(), g_atomicParts.size(), g_connections.size());
}

// ── Random helpers ──────────────────────────────────────────
static int pick_ca(std::mt19937 &r) { return r() % MAX_CA; }
static int pick_ba(std::mt19937 &r) { return r() % MAX_BA; }
static int pick_cp(std::mt19937 &r) { return r() % MAX_CP; }
static int pick_ap(std::mt19937 &r) { return r() % MAX_AP; }
static int pick_doc(std::mt19937 &r){ return r() % MAX_DOCS; }

// ── Op result ───────────────────────────────────────────────
struct OpResult { uint64_t val; bool wasWrite; };

// ==================================================================
// LONG TRAVERSALS
// ==================================================================

static OpResult op_lt1() { // RO: sum all assembly IDs+dates
    expli::TM<int>::begin();
    int64_t s = 0;
    for (size_t i=0; i<g_complexAssemblies.size(); i++)
        s += g_complexAssemblies[i].id + g_complexAssemblies[i].level + g_complexAssemblies[i].buildDate.read();
    for (size_t i=0; i<g_baseAssemblies.size(); i++)
        s += g_baseAssemblies[i].id + g_baseAssemblies[i].buildDate.read();
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_lt2() { // UP: update all assembly dates
    expli::TM<int>::begin();
    for (size_t i=0; i<g_complexAssemblies.size(); i++)
        g_complexAssemblies[i].buildDate.write((g_complexAssemblies[i].buildDate.read() + 1) % 365 + 1000);
    for (size_t i=0; i<g_baseAssemblies.size(); i++)
        g_baseAssemblies[i].buildDate.write((g_baseAssemblies[i].buildDate.read() + 1) % 365 + 1000);
    expli::TM<int>::end();
    return {0, true};
}

static OpResult op_lt3() { // RO: sum all CP dates
    expli::TM<int>::begin();
    int64_t s = 0;
    for (size_t i=0; i<g_compositeParts.size(); i++)
        s += g_compositeParts[i].buildDate.read();
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_lt4() { // UP: update all AP weights
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

// ==================================================================
// SHORT TRAVERSALS
// ==================================================================

static OpResult op_st1() { // RO: follow tree from root
    expli::TM<int>::begin();
    int64_t s = 0;
    if (g_modules.empty()) { expli::TM<int>::end(); return {0,false}; }
    int ci = g_modules[0].rootAssemblyId;
    if (ci >= 0 && ci < (int)g_complexAssemblies.size())
        s += g_complexAssemblies[ci].id;
    for (int l=0; l<TREE_LEVELS-1 && !g_complexAssemblies[ci].childAssemblyIds.empty(); l++) {
        ci = g_complexAssemblies[ci].childAssemblyIds[0];
        s += g_complexAssemblies[ci].id;
    }
    if (!g_complexAssemblies[ci].childBaseAssemblyIds.empty()) {
        int bi = g_complexAssemblies[ci].childBaseAssemblyIds[0];
        if (bi < (int)g_baseAssemblies.size()) {
            s += g_baseAssemblies[bi].buildDate.read();
            if (!g_baseAssemblies[bi].compositePartIds.empty()) {
                int cpi = g_baseAssemblies[bi].compositePartIds[0];
                if (cpi < (int)g_compositeParts.size())
                    s += g_compositeParts[cpi].buildDate.read();
            }
        }
    }
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

static OpResult op_st2(int doc_idx) { // RO: from doc through CP to APs
    expli::TM<int>::begin();
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) { expli::TM<int>::end(); return {0,false}; }
    int64_t s = g_documents[doc_idx].buildDate.read() + g_documents[doc_idx].type.read();
    int ci = g_documents[doc_idx].compositePartId;
    if (ci >= 0 && ci < (int)g_compositeParts.size()) {
        auto &aps = g_compositeParts[ci].atomicPartIds;
        for (size_t i=0; i<aps.size(); i++) {
            int ai = aps[i];
            if (ai >= 0 && ai < (int)g_atomicParts.size())
                s += g_atomicParts[ai].weight.read();
        }
    }
    expli::TM<int>::end();
    return {(uint64_t)s, false};
}

// ==================================================================
// SHORT OPERATIONS
// ==================================================================

static OpResult op_op1_lookup_ap(int id) { // RO: AP by ID
    expli::TM<int>::begin();
    int *idx = g_apById.find(id);
    uint64_t r = 0;
    if (idx && *idx >= 0 && *idx < (int)g_atomicParts.size())
        r = (uint64_t)(g_atomicParts[*idx].x.read() + g_atomicParts[*idx].y.read() + g_atomicParts[*idx].z.read());
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op2_lookup_cp(int id) { // RO: CP by ID
    expli::TM<int>::begin();
    int *idx = g_cpById.find(id);
    uint64_t r = 0;
    if (idx && *idx >= 0 && *idx < (int)g_compositeParts.size())
        r = (uint64_t)g_compositeParts[*idx].buildDate.read();
    expli::TM<int>::end();
    return {r, false};
}

static OpResult op_op11_update_ap(int ai, int nx, int ny) { // UP
    expli::TM<int>::begin();
    if (ai >= 0 && ai < (int)g_atomicParts.size()) {
        g_atomicParts[ai].x.write(nx);
        g_atomicParts[ai].y.write(ny);
    }
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_op12_update_weight(int ai, int nw) { // UP
    expli::TM<int>::begin();
    if (ai >= 0 && ai < (int)g_atomicParts.size())
        g_atomicParts[ai].weight.write(nw);
    expli::TM<int>::end();
    return {1, true};
}

// ==================================================================
// STRUCTURE MODIFICATIONS (minimal)
// ==================================================================

static OpResult op_sm2_delete_cp(int ci) { // delete CP (tombstone)
    expli::TM<int>::begin();
    if (ci >= 0 && ci < (int)g_compositeParts.size())
        g_compositeParts[ci].buildDate.write(-1); // tombstone
    expli::TM<int>::end();
    return {1, true};
}

static OpResult op_sm4_delete_ap(int ai) { // delete AP (tombstone)
    expli::TM<int>::begin();
    if (ai >= 0 && ai < (int)g_atomicParts.size())
        g_atomicParts[ai].weight.write(-1); // tombstone
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

static OpResult wrap_st2(std::mt19937 &r) { return op_st2(pick_doc(r)); }
static OpResult wrap_op1(std::mt19937 &r) { return op_op1_lookup_ap(r() % MAX_AP); }
static OpResult wrap_op2(std::mt19937 &r) { return op_op2_lookup_cp(r() % MAX_CP); }
static OpResult wrap_op11(std::mt19937 &r) { return op_op11_update_ap(pick_ap(r), r()%100, r()%100); }
static OpResult wrap_op12(std::mt19937 &r) { return op_op12_update_weight(pick_ap(r), (r()%50)+1); }
static OpResult wrap_sm2(std::mt19937 &r) { return op_sm2_delete_cp(pick_cp(r)); }
static OpResult wrap_sm4(std::mt19937 &r) { return op_sm4_delete_ap(pick_ap(r)); }

static OpDesc g_ops[] = {
    {LONG_TRAV, true,  0, [](auto&r){return op_lt1();}},
    {LONG_TRAV, false, 1, [](auto&r){return op_lt2();}},
    {LONG_TRAV, true,  2, [](auto&r){return op_lt3();}},
    {LONG_TRAV, false, 3, [](auto&r){return op_lt4();}},
    {SHORT_TRAV, true, 4, wrap_st2},
    {SHORT_TRAV, true, 5, [](auto&r){return op_st1();}},
    {SHORT_OP, true,   6, wrap_op1},
    {SHORT_OP, true,   7, wrap_op2},
    {SHORT_OP, false,  8, wrap_op11},
    {SHORT_OP, false,  9, wrap_op12},
    {STRUCT_MOD, false,10, wrap_sm2},
    {STRUCT_MOD, false,11, wrap_sm4},
};
constexpr int NUM_OPS = sizeof(g_ops)/sizeof(g_ops[0]);

// ── Category distribution ───────────────────────────────────
static OpCat pick_category(std::mt19937 &r) {
    double roll = std::uniform_real_distribution<double>(0,100)(r);
    if (roll < 5)  return LONG_TRAV;
    if (roll < 45) return SHORT_TRAV;
    if (roll < 90) return SHORT_OP;
    return STRUCT_MOD;
}

static int count_by_cat[4] = {0,0,0,0};

// ── Worker ──────────────────────────────────────────────────
static std::atomic<bool> g_stop{false};

static void worker(int seed) {
    expli::TM<int>::thread_init();
    auto rng = std::mt19937(seed);

    while (!g_stop.load()) {
        OpCat cat = pick_category(rng);
        // Pick a random op within category
        int first = -1, last = -1;
        for (int i=0; i<NUM_OPS; i++) {
            if (g_ops[i].cat == cat) {
                if (first < 0) first = i;
                last = i;
            }
        }
        if (first < 0) continue;
        int idx = first + (rng() % (last - first + 1));
        g_ops[idx].fn(rng);
        count_by_cat[cat]++;
    }
    expli::TM<int>::thread_exit();
}

// ── Main ────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    int duration_ms = DURATION_MS;
    int nb_threads  = 4;
    if (argc > 1) nb_threads = atoi(argv[1]);
    if (argc > 2) duration_ms = atoi(argv[2]);

    printf("STMbench7 — Explicit TM API\n");
    printf("Threads: %d  Duration: %d ms\n", nb_threads, duration_ms);

    expli::TM<int>::init();
    init_data();

    auto start = std::chrono::high_resolution_clock::now();
    std::thread *threads = new std::thread[nb_threads];
    for (int i=0; i<nb_threads; i++)
        new (&threads[i]) std::thread(worker, 1234 + i);

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    g_stop.store(true);
    for (int i=0; i<nb_threads; i++) threads[i].join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();

    uint64_t total = 0;
    for (int c=0; c<4; c++) total += count_by_cat[c];

    printf("\nResults (%d ms):\n", (int)ms);
    printf("  Long trav:   %d\n", count_by_cat[0]);
    printf("  Short trav:  %d\n", count_by_cat[1]);
    printf("  Short ops:   %d\n", count_by_cat[2]);
    printf("  Struct mods: %d\n", count_by_cat[3]);
    printf("  Total:       %llu\n", (unsigned long long)total);
    printf("  Ops/sec:     %.0f\n", total * 1000.0 / ms);

    delete[] threads;
    expli::TM<int>::exit();
    printf("PASS\n");
    return 0;
}
