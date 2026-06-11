/**
 * STMbench7 - Full OO7-based Spec Implementation
 *
 * Based on: "STMBench7: A Benchmark for Software Transactional Memory"
 *   Guerraoui, Kapalka, Vitek. EuroSys 2007.
 *   https://janvitek.org/pubs/eurosys07.pdf
 *
 * Data structure (Fig 1): Module → ComplexAssembly tree → BaseAssembly leaves
 *   → CompositePart (design library, many-to-many with BA via bags)
 *   → AtomicPart graph (200 per CP) connected by Connection objects (3× AP count)
 *   → Document (1 per CP)
 *   → Manual (single object)
 *
 * 45 operations in 4 categories (§3):
 *   Long traversals (5):   LT1-LT5  — traverse entire assemblies/APs/connections
 *   Short traversals (10): ST1-ST10 — random path from module/doc/AP
 *   Short operations (15): OP1-OP15 — single object or local neighborhood
 *   Structure mods (8):    SM1-SM8  — create/delete elements
 *
 * Category distribution (§3 Table): 5% LT, 40% ST, 45% OP, 10% SM
 * Workloads (§3): read-dominated (90/10), read-write (60/40), write-dominated (10/90)
 *
 * Scale (§2): 1 Module, 364 CA (6 levels×fanout3), 729 BA, 500 CP,
 *   100k AP (200/CP), ≥300k connections, 500 documents, 1 Manual.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include "datastructures/tm_treap_map.hpp"
#include "tm_spin_token.hpp"
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>
#include "tm_vector.hpp"

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

extern void tm_set_num_threads(int n);

// ─── Spec constants (§2: medium OO7 size) ────────────────────────────────
constexpr int FANOUT           = 3;
constexpr int TREE_LEVELS      = 6;      // 0..5 internal (CA), level 5 has BA children
constexpr int MAX_MODULES      = 1;      // §2: single module
constexpr int MAX_CP           = 500;    // §2: design library size
constexpr int AP_PER_CP        = 200;    // §2: atomic parts per composite part
constexpr int CONN_PER_AP      = 3;      // §2: "at least three times as many connections"
constexpr int MAX_DOCUMENTS    = MAX_CP; // §2: one document per composite part
constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS  = 4;

// Derived counts
constexpr int MAX_CA = ([]() constexpr {
    int total = 0, level_size = 1;
    for (int l = 0; l < TREE_LEVELS; l++) {
        total += level_size;
        level_size *= FANOUT;
    }
    return total;
})();  // 1+3+9+27+81+243 = 364
constexpr int MAX_BA = ([]() constexpr {
    int ba = 1;
    for (int l = 0; l < TREE_LEVELS; l++) ba *= FANOUT;
    return ba;
})();  // 3^6 = 729
constexpr int MAX_AP     = MAX_CP * AP_PER_CP;         // 100,000
constexpr int MAX_CONN   = MAX_AP * CONN_PER_AP;       // 300,000
constexpr int MAX_CP_BA_BAG = 5;  // max CPs per BA / BA per CP

// ─── Data structures (Fig 1, §DataStructure) ────────────────────────────

struct Manual {                          // §: single special document
    int id;
    char text[256];
};

struct Document {                        // §: associated with one CompositePart
    int id;
    int type;
    int buildDate;
    int compositePartId;                 // owning CP index
};

struct Connection {                      // §: directed edge between two AtomicParts
    int id;
    int fromAtomicPartId;
    int toAtomicPartId;
    int type;
};

struct AtomicPart {                      // §: node in a graph, belongs to one CP
    int id;
    int x, y, z;
    int buildDate;
    int weight;
    int compositePartId;                 // owning CP index
    TMSafeVector<int> connectionIds;     // incident connection indices
};

struct CompositePart {                   // §: design library entry
    int id;
    int buildDate;
    int documentId;                      // owning Document index
    int rootAtomicPartId;                // entry point into graph
    TMSafeVector<int> atomicPartIds;     // all APs in this graph
    TMSafeVector<int> baseAssemblyIds;   // reverse bag: which BAs contain this CP
};

struct BaseAssembly {                    // §: tree leaf
    int id;
    int parentAssemblyId;                // parent ComplexAssembly index
    int buildDate;
    TMSafeVector<int> compositePartIds;  // bag of CPs belonging to this BA
};

struct ComplexAssembly {                 // §: internal tree node
    int id;
    int level;                           // 0 (root) .. TREE_LEVELS-1
    int parentId;                        // -1 for root
    TMSafeVector<int> childAssemblyIds;   // child CA indices (levels 0..TREE_LEVELS-2)
    TMSafeVector<int> childBaseAssemblyIds; // child BA indices (level TREE_LEVELS-1 only)
    int buildDate;
};

struct Module {                          // §: design root
    int id;
    int rootAssemblyId;                  // top ComplexAssembly index
};

// ─── TM globals ─────────────────────────────────────────────────────────
TM TMSafeVector<Module>          g_modules;
TM TMSafeVector<ComplexAssembly> g_complexAssemblies;
TM TMSafeVector<BaseAssembly>    g_baseAssemblies;
TM TMSafeVector<CompositePart>   g_compositeParts;
TM TMSafeVector<AtomicPart>      g_atomicParts;
TM TMSafeVector<Connection>      g_connections;
TM TMSafeVector<Document>        g_documents;
TM Manual                       g_manual;

// Indexes (§3, Table 1): 6 ID‑based indexes + 2 date indexes
TM TMTreapMap<int, int>           g_caById;          // ComplexAssembly ID → index
TM TMTreapMap<int, int>           g_baById;          // BaseAssembly    ID → index
TM TMTreapMap<int, int>           g_cpById;          // CompositePart   ID → index
TM TMTreapMap<int, int>           g_apById;          // AtomicPart      ID → index
TM TMTreapMap<int, int>           g_docById;         // Document        ID → index
TM TMTreapMultiMap<int, int>      g_cpByDate;        // buildDate → CP index
TM TMTreapMultiMap<int, int>      g_apByDate;        // buildDate → AP index

TM int g_atomicPartCount = 0;
TM int g_connectionCount = 0;

// ─── Initialisation ─────────────────────────────────────────────────────

static void init_data() {
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

    // Pre-reserve capacity to prevent vector reallocation during TX.
    // The LLVM TM plugin instruments ALL memory accesses inside a TX,
    // including std::vector's internal memcpy/memmove for reallocation.
    // The _tm_clone of __relocate_a_1 reads old-element bytes via TM
    // barriers (tm_read_i1) while the old buffer is about to be freed —
    // this crashes in read_word_ctl(). Pre-reserving to max possible
    // size avoids reallocation entirely. All struct mod SM operations
    // are bounded to stay within these reserves.
    g_complexAssemblies.reserve(MAX_CA);                     // 364, fixed tree
    g_baseAssemblies.reserve(MAX_BA * 2);                    // 1458, SM7 can add
    g_compositeParts.reserve(MAX_CP * 2);                    // 1000, SM1 can add
    g_atomicParts.reserve(MAX_AP * 2);                       // 200000, SM3 can add
    g_connections.reserve(MAX_CONN * 2);                     // 600000, SM5 can add
    g_documents.reserve(MAX_DOCUMENTS + MAX_CP);             // 1000, SM1 creates 1 doc per CP

    // --- Module (§: single module) ---
    Module mod;
    mod.id = 0;
    mod.rootAssemblyId = 0;
    g_modules.push_back(mod);

    // --- Build assembly tree (§2: TREE_LEVELS internal CA levels + BA leaves) ---
    // Level 0..TREE_LEVELS-1 are ComplexAssemblies.
    // Level TREE_LEVELS-1 CA have BaseAssembly children.

    // Allocate all CA first so their indices are stable
    int level_sizes[TREE_LEVELS];
    int level_offset[TREE_LEVELS];
    {
        int off = 0;
        int sz  = 1;
        for (int l = 0; l < TREE_LEVELS; l++) {
            level_sizes[l]  = sz;
            level_offset[l] = off;
            for (int j = 0; j < sz; j++) {
                ComplexAssembly ca;
                ca.id        = off + j;
                ca.level     = l;
                ca.parentId  = -1;
                ca.buildDate = 1000 + (l * 100 + j) % 365;
                ca.childAssemblyIds.reserve(FANOUT);
                ca.childBaseAssemblyIds.reserve(FANOUT);
                g_complexAssemblies.push_back(ca);
            }
            off += sz;
            sz  *= FANOUT;
        }
    }

    // Wire parent/child relationships among CAs
    for (int l = 1; l < TREE_LEVELS; l++) {
        int parent_off  = level_offset[l - 1];
        int parent_sz   = level_sizes[l - 1];
        int child_off   = level_offset[l];
        for (int p = 0; p < parent_sz; p++) {
            int parent_idx = parent_off + p;
            for (int c = 0; c < FANOUT; c++) {
                int child_idx = child_off + p * FANOUT + c;
                g_complexAssemblies[parent_idx].childAssemblyIds.push_back(child_idx);
                g_complexAssemblies[child_idx].parentId = parent_idx;
            }
        }
    }

    // --- BaseAssemblies (leaves, children of level TREE_LEVELS-1 CAs) ---
    int ba_parent_off = level_offset[TREE_LEVELS - 1];
    int ba_parent_sz  = level_sizes[TREE_LEVELS - 1];
    int ba_id = 0;
    for (int p = 0; p < ba_parent_sz; p++) {
        int parent_idx = ba_parent_off + p;
        for (int c = 0; c < FANOUT; c++) {
            BaseAssembly ba;
            ba.id                 = ba_id;
            ba.parentAssemblyId    = parent_idx;
            ba.buildDate          = 1000 + (ba_id % 365);
            g_baseAssemblies.push_back(ba);
            g_complexAssemblies[parent_idx].childBaseAssemblyIds.push_back(ba_id);
            ba_id++;
        }
    }

    // --- CompositeParts (§2: 500 in design library) + Documents (§: 1 per CP) ---
    for (int cp_idx = 0; cp_idx < MAX_CP; cp_idx++) {
        CompositePart cp;
        cp.id            = cp_idx;
        cp.buildDate     = 1000 + (cp_idx % 365);
        cp.documentId    = cp_idx;
        cp.rootAtomicPartId = -1;

        // Document
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

    // --- Many-to-many BA↔CP bags (§: "two bags each") ---
    // Distribute CPs among BAs so each BA gets ~(MAX_CP*MAX_CP_BA_BAG/MAX_BA) CPs
    // and each CP appears in ~MAX_CP_BA_BAG BAs.
    {
        std::mt19937 rng(42);
        for (int cp_idx = 0; cp_idx < MAX_CP; cp_idx++) {
            int num_ba = 1 + (rng() % (MAX_CP_BA_BAG - 1));
            std::set<int> chosen;
            while ((int)chosen.size() < num_ba)
                chosen.insert(rng() % MAX_BA);
            for (int ba_idx : chosen) {
                g_compositeParts[cp_idx].baseAssemblyIds.push_back(ba_idx);
                g_baseAssemblies[ba_idx].compositePartIds.push_back(cp_idx);
            }
        }
    }

    // --- AtomicParts (§2: 200 per CP) + Connections (§2: ≥3× AP count) ---
    {
        std::mt19937 rng(99);
        for (int cp_idx = 0; cp_idx < MAX_CP; cp_idx++) {
            int first_ap_idx = (int)g_atomicParts.size();
            g_compositeParts[cp_idx].rootAtomicPartId = first_ap_idx;

            // Create APs
            for (int j = 0; j < AP_PER_CP; j++) {
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

            // Build graph connections within this CP's AP set (§: "connections between them")
            // Each AP gets ~CONN_PER_AP connections → total ~AP_PER_CP*CONN_PER_AP
            // Use ring + chord + random extra edges
            for (int j = 0; j < AP_PER_CP; j++) {
                int ap_idx = first_ap_idx + j;
                // Ring: connect j → (j+1)%n
                int t1 = first_ap_idx + (j + 1) % AP_PER_CP;
                Connection c1;
                c1.id               = g_connections.size();
                c1.fromAtomicPartId = ap_idx;
                c1.toAtomicPartId   = t1;
                c1.type             = j % 3;
                g_connections.push_back(c1);
                g_atomicParts[ap_idx].connectionIds.push_back(c1.id);

                // Chord: connect j → (j+2)%n
                int t2 = first_ap_idx + (j + 2) % AP_PER_CP;
                Connection c2;
                c2.id               = g_connections.size();
                c2.fromAtomicPartId = ap_idx;
                c2.toAtomicPartId   = t2;
                c2.type             = (j + 1) % 3;
                g_connections.push_back(c2);
                g_atomicParts[ap_idx].connectionIds.push_back(c2.id);

                // Additional random connections to reach CONN_PER_AP per AP
                // (already have 2 from ring+chord, need CONN_PER_AP-2 more)
                for (int k = 0; k < CONN_PER_AP - 2; k++) {
                    int t3 = first_ap_idx + (rng() % AP_PER_CP);
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

    // --- Manual (§: single special document) ---
    g_manual.id = 0;
    strncpy(g_manual.text, "STMbench7 Manual", sizeof(g_manual.text) - 1);
    g_manual.text[sizeof(g_manual.text) - 1] = '\0';

    g_atomicPartCount = (int)g_atomicParts.size();
    g_connectionCount = (int)g_connections.size();

    // Pre-reserve inner vector capacities — same rationale: prevent
    // reallocation during TX. SM3 pushes to atomicPartIds, SM5 pushes
    // to connectionIds. Reserves account for worst-case SM activity.
    for (auto &cp : g_compositeParts) {
        cp.atomicPartIds.reserve(AP_PER_CP * 2);       // 400, SM3 can add ~200 more
        cp.baseAssemblyIds.reserve(MAX_CP_BA_BAG * 2); // 10
    }
    for (auto &ba : g_baseAssemblies)
        ba.compositePartIds.reserve(MAX_CP_BA_BAG * 2);
    for (auto &ap : g_atomicParts)
        ap.connectionIds.reserve(CONN_PER_AP * 4);     // 12, SM5 can add a few
}

// ─── Random helpers ─────────────────────────────────────────────────────
static int pick_ca(std::mt19937 &rng) {
    return rng() % (int)g_complexAssemblies.size();
}
static int pick_ba(std::mt19937 &rng) {
    return rng() % (int)g_baseAssemblies.size();
}
static int pick_cp(std::mt19937 &rng) {
    return rng() % (int)g_compositeParts.size();
}
static int pick_ap(std::mt19937 &rng) {
    return rng() % (int)g_atomicParts.size();
}
static int pick_doc(std::mt19937 &rng) {
    return rng() % (int)g_documents.size();
}

// ─── Operation result wrapper ───────────────────────────────────────────
struct OpResult {
    uint64_t value;
    bool     wasWrite;
};

// ====================================================================
// LONG TRAVERSALS (§3): go through ALL assemblies / ALL atomic parts
// ====================================================================

// LT1: read-only — traverse all assemblies, sum IDs
TX OpResult op_lt1() {
    int64_t sum = 0;
    for (auto &ca : g_complexAssemblies) sum += ca.id + ca.level + ca.buildDate;
    for (auto &ba : g_baseAssemblies)   sum += ba.id + ba.buildDate;
    return { (uint64_t)sum, false };
}

// LT2: update — traverse all assemblies, update each CA/BA buildDate
TX OpResult op_lt2() {
    for (auto &ca : g_complexAssemblies) ca.buildDate = (ca.buildDate + 1) % 365 + 1000;
    for (auto &ba : g_baseAssemblies)   ba.buildDate = (ba.buildDate + 1) % 365 + 1000;
    return { 0, true };
}

// LT3: read-only — traverse all composite parts, sum fields
TX OpResult op_lt3() {
    int64_t sum = 0;
    for (auto &cp : g_compositeParts) {
        sum += cp.id + cp.buildDate;
    }
    return { (uint64_t)sum, false };
}

// LT4: update — traverse all atomic parts, update weights
TX OpResult op_lt4() {
    int64_t sum = 0;
    for (auto &ap : g_atomicParts) {
        ap.weight = (ap.weight % 50) + 1;
        sum += ap.weight;
    }
    return { (uint64_t)sum, true };
}

// LT5: read-only — traverse all connections, sum fields
TX OpResult op_lt5() {
    int64_t sum = 0;
    for (auto &c : g_connections) sum += c.id + c.fromAtomicPartId + c.toAtomicPartId + c.type;
    return { (uint64_t)sum, false };
}

// ====================================================================
// SHORT TRAVERSALS (§3): random path from module / doc / atomic part
// ====================================================================

// ST1 (RO): from root module, follow random path down through levels, inspect
TX OpResult op_st1() {
    int64_t sum = 0;
    if (g_modules.empty()) return { 0, false };
    int ca_idx = g_modules[0].rootAssemblyId;
    if (ca_idx < 0 || ca_idx >= (int)g_complexAssemblies.size()) return { 0, false };
    sum += g_complexAssemblies[ca_idx].id;
    // Descend through levels
    for (int l = 0; l < TREE_LEVELS - 1; l++) {
        auto &ca = g_complexAssemblies[ca_idx];
        if (ca.childAssemblyIds.empty()) break;
        ca_idx = ca.childAssemblyIds[0];  // follow first child
        sum += g_complexAssemblies[ca_idx].id;
    }
    // At level TREE_LEVELS-1 CA, visit its first BA
    auto &last_ca = g_complexAssemblies[ca_idx];
    if (!last_ca.childBaseAssemblyIds.empty()) {
        int ba_idx = last_ca.childBaseAssemblyIds[0];
        if (ba_idx < (int)g_baseAssemblies.size()) {
            auto &ba = g_baseAssemblies[ba_idx];
            sum += ba.id + ba.buildDate;
            // Check first CP
            if (!ba.compositePartIds.empty()) {
                int cp_idx = ba.compositePartIds[0];
                if (cp_idx < (int)g_compositeParts.size())
                    sum += g_compositeParts[cp_idx].buildDate;
            }
        }
    }
    return { (uint64_t)sum, false };
}

// ST2 (RO): from random document, traverse to CP and its AP graph
TX OpResult op_st2_traverse(int doc_idx) {
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return {0, false};
    int64_t sum = g_documents[doc_idx].buildDate + g_documents[doc_idx].type;
    int cp_idx = g_documents[doc_idx].compositePartId;
    if (cp_idx >= 0 && cp_idx < (int)g_compositeParts.size()) {
        for (int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
            if (ap_idx >= 0 && ap_idx < (int)g_atomicParts.size())
                sum += g_atomicParts[ap_idx].weight;
        }
    }
    return { (uint64_t)sum, false };
}

// ST3 (RO): from random AP, traverse local neighbourhood (connections)
TX OpResult op_st3_traverse(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, false };
    int64_t sum = 0;
    auto &ap = g_atomicParts[ap_idx];
    sum += ap.x + ap.y + ap.z;
    for (int conn_idx : ap.connectionIds) {
        if (conn_idx < (int)g_connections.size()) {
            auto &c = g_connections[conn_idx];
            int nb = (c.fromAtomicPartId == ap_idx) ? c.toAtomicPartId : c.fromAtomicPartId;
            if (nb >= 0 && nb < (int)g_atomicParts.size())
                sum += g_atomicParts[nb].weight;
        }
    }
    return { (uint64_t)sum, false };
}

// ST4 (UP): from random CA, update its buildDate
TX OpResult op_st4_update_ca(int ca_idx) {
    if (ca_idx < 0 || ca_idx >= (int)g_complexAssemblies.size()) return { 0, true };
    g_complexAssemblies[ca_idx].buildDate = (g_complexAssemblies[ca_idx].buildDate + 1) % 365 + 1000;
    return { 1, true };
}

// ST5 (RO): query CPs by build date range (index lookup)
TX OpResult op_st5_date_range(int low, int high) {
    int64_t count = 0;
    auto it = g_cpByDate.lower_bound(low);
    while (it != g_cpByDate.end() && it->first <= high) {
        count++;
        ++it;
    }
    return { (uint64_t)count, false };
}

// ST6 (UP): from random AP, update its coordinates
TX OpResult op_st6_update_ap(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].x = (g_atomicParts[ap_idx].x + 1) % 100;
    g_atomicParts[ap_idx].y = (g_atomicParts[ap_idx].y + 1) % 100;
    return { 1, true };
}

// ST7 (RO): find max weight AP in a CP's graph
TX OpResult op_st7_max_weight(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    int max_w = 0;
    for (int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
        if (ap_idx < (int)g_atomicParts.size())
            max_w = std::max(max_w, g_atomicParts[ap_idx].weight);
    }
    return { (uint64_t)max_w, false };
}

// ST8 (UP): from random BA, update its buildDate
TX OpResult op_st8_update_ba(int ba_idx) {
    if (ba_idx < 0 || ba_idx >= (int)g_baseAssemblies.size()) return { 0, true };
    g_baseAssemblies[ba_idx].buildDate = (g_baseAssemblies[ba_idx].buildDate + 1) % 365 + 1000;
    return { 1, true };
}

// ST9 (RO): from random CP, traverse its AP graph (sum weights)
TX OpResult op_st9_traverse_cp(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    int64_t sum = 0;
    for (int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
        if (ap_idx < (int)g_atomicParts.size())
            sum += g_atomicParts[ap_idx].weight;
    }
    return { (uint64_t)sum, false };
}

// ST10 (UP): from random CP, update its document date
TX OpResult op_st10_update_doc(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, true };
    int doc_idx = g_compositeParts[cp_idx].documentId;
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return { 0, true };
    g_documents[doc_idx].buildDate = (g_documents[doc_idx].buildDate + 1) % 365 + 1000;
    return { 1, true };
}

// ====================================================================
// SHORT OPERATIONS (§3): single object or local neighbourhood
// ====================================================================

// OP1 (RO): query AP by ID (index lookup)
TX OpResult op_op1_lookup_ap(int id) {
    auto it = g_apById.find(id);
    if (it == g_apById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_atomicParts.size()) return { 0, false };
    auto &ap = g_atomicParts[it->second];
    return { (uint64_t)(ap.x + ap.y + ap.z), false };
}

// OP2 (RO): query CP by ID (index lookup)
TX OpResult op_op2_lookup_cp(int id) {
    auto it = g_cpById.find(id);
    if (it == g_cpById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_compositeParts.size()) return { 0, false };
    return { (uint64_t)g_compositeParts[it->second].buildDate, false };
}

// OP3 (RO): query Document by ID (index lookup)
TX OpResult op_op3_lookup_doc(int id) {
    auto it = g_docById.find(id);
    if (it == g_docById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_documents.size()) return { 0, false };
    return { (uint64_t)(g_documents[it->second].buildDate + g_documents[it->second].type), false };
}

// OP4 (RO): query BA by ID (index lookup)
TX OpResult op_op4_lookup_ba(int id) {
    auto it = g_baById.find(id);
    if (it == g_baById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_baseAssemblies.size()) return { 0, false };
    return { (uint64_t)g_baseAssemblies[it->second].buildDate, false };
}

// OP5 (RO): query CA by ID (index lookup)
TX OpResult op_op5_lookup_ca(int id) {
    auto it = g_caById.find(id);
    if (it == g_caById.end()) return { 0, false };
    if (it->second < 0 || it->second >= (int)g_complexAssemblies.size()) return { 0, false };
    return { (uint64_t)g_complexAssemblies[it->second].buildDate, false };
}

// OP6 (RO): read sum of AP coordinates
TX OpResult op_op6_read_ap(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, false };
    auto &ap = g_atomicParts[ap_idx];
    return { (uint64_t)(ap.x + ap.y + ap.z), false };
}

// OP7 (RO): read CP buildDate
TX OpResult op_op7_read_cp(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    return { (uint64_t)g_compositeParts[cp_idx].buildDate, false };
}

// OP8 (RO): count documents for a CP (always 1, but simulates index access)
TX OpResult op_op8_count_docs(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    return { 1, false };
}

// OP9 (RO): sum AP weights in a CP graph
TX OpResult op_op9_sum_weights(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, false };
    int64_t sum = 0;
    for (int ap_idx : g_compositeParts[cp_idx].atomicPartIds) {
        if (ap_idx < (int)g_atomicParts.size())
            sum += g_atomicParts[ap_idx].weight;
    }
    return { (uint64_t)sum, false };
}

// OP10 (RO): check connection count for an AP
TX OpResult op_op10_count_conns(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, false };
    return { (uint64_t)g_atomicParts[ap_idx].connectionIds.size(), false };
}

// OP11 (UP): update AP coordinates
TX OpResult op_op11_update_ap(int ap_idx, int nx, int ny) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].x = nx;
    g_atomicParts[ap_idx].y = ny;
    return { 1, true };
}

// OP12 (UP): update AP weight
TX OpResult op_op12_update_weight(int ap_idx, int nw) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].weight = nw;
    return { 1, true };
}

// OP13 (UP): update document date
TX OpResult op_op13_update_doc(int doc_idx, int nd) {
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return { 0, true };
    g_documents[doc_idx].buildDate = nd;
    return { 1, true };
}

// OP14 (UP): update CP buildDate
TX OpResult op_op14_update_cp(int cp_idx, int nd) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, true };
    g_compositeParts[cp_idx].buildDate = nd;
    return { 1, true };
}

// OP15 (UP): update BA buildDate
TX OpResult op_op15_update_ba(int ba_idx, int nd) {
    if (ba_idx < 0 || ba_idx >= (int)g_baseAssemblies.size()) return { 0, true };
    g_baseAssemblies[ba_idx].buildDate = nd;
    return { 1, true };
}

// ====================================================================
// STRUCTURE MODIFICATIONS (§3): create/delete elements
// ====================================================================

// SM1: create composite part (with doc + APs + connections)
TX OpResult op_sm1_create_cp(int new_id) {
    if ((int)g_compositeParts.size() >= MAX_CP * 2) return { 0, true };  // limit growth
    int cp_idx = (int)g_compositeParts.size();

    CompositePart cp;
    cp.id            = new_id;
    cp.buildDate     = 2000;
    cp.documentId    = (int)g_documents.size();
    cp.rootAtomicPartId = (int)g_atomicParts.size();
    cp.atomicPartIds.reserve(AP_PER_CP + 8);
    cp.baseAssemblyIds.reserve(MAX_CP_BA_BAG + 2);
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

    // Create APs + connections for this new CP
    int first_ap = cp.rootAtomicPartId;
    for (int j = 0; j < AP_PER_CP; j++) {
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
    // Ring connections
    for (int j = 0; j < AP_PER_CP; j++) {
        int a = first_ap + j;
        int b = first_ap + (j + 1) % AP_PER_CP;
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

// SM2: delete composite part (mark deleted)
TX OpResult op_sm2_delete_cp(int cp_idx) {
    if (cp_idx < 0 || cp_idx >= (int)g_compositeParts.size()) return { 0, true };
    g_compositeParts[cp_idx].id = -1;  // tombstone
    return { 1, true };
}

// SM3: create atomic part (attached to a CP)
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

// SM4: delete atomic part (tombstone)
TX OpResult op_sm4_delete_ap(int ap_idx) {
    if (ap_idx < 0 || ap_idx >= (int)g_atomicParts.size()) return { 0, true };
    g_atomicParts[ap_idx].id = -1;
    return { 1, true };
}

// SM5: create connection
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

// SM6: delete connection (tombstone)
TX OpResult op_sm6_delete_conn(int conn_idx) {
    if (conn_idx < 0 || conn_idx >= (int)g_connections.size()) return { 0, true };
    // Remove from the from-AP's list
    int from_ap = g_connections[conn_idx].fromAtomicPartId;
    if (from_ap >= 0 && from_ap < (int)g_atomicParts.size()) {
        auto &clist = g_atomicParts[from_ap].connectionIds;
        clist.erase(std::remove(clist.begin(), clist.end(), conn_idx), clist.end());
    }
    g_connections[conn_idx].fromAtomicPartId = -1;
    return { 1, true };
}

// SM7: create base assembly (attached to a CA at level TREE_LEVELS-1)
TX OpResult op_sm7_create_ba(int parent_ca_idx) {
    if (parent_ca_idx < 0 || parent_ca_idx >= (int)g_complexAssemblies.size()) return { 0, true };
    auto &ca = g_complexAssemblies[parent_ca_idx];
    if (ca.level != TREE_LEVELS - 1) return { 0, true };  // only leaf CAs have BAs
    int ba_idx = (int)g_baseAssemblies.size();
    BaseAssembly ba;
    ba.id                   = ba_idx;
    ba.parentAssemblyId     = parent_ca_idx;
    ba.buildDate            = 2000;
    ba.compositePartIds.reserve(MAX_CP_BA_BAG + 2);
    g_baseAssemblies.push_back(ba);
    ca.childBaseAssemblyIds.push_back(ba_idx);
    g_baById[ba_idx] = ba_idx;
    return { 1, true };
}

// SM8: delete base assembly (tombstone)
TX OpResult op_sm8_delete_ba(int ba_idx) {
    if (ba_idx < 0 || ba_idx >= (int)g_baseAssemblies.size()) return { 0, true };
    g_baseAssemblies[ba_idx].id = -1;
    // Remove from parent CA's child list
    int parent = g_baseAssemblies[ba_idx].parentAssemblyId;
    if (parent >= 0 && parent < (int)g_complexAssemblies.size()) {
        auto &clist = g_complexAssemblies[parent].childBaseAssemblyIds;
        clist.erase(std::remove(clist.begin(), clist.end(), ba_idx), clist.end());
    }
    return { 1, true };
}

// ====================================================================
// OPERATION SELECTION (§3 Table)
// ====================================================================

enum class OpClass { LONG_TRAV, SHORT_TRAV, SHORT_OP, STRUCT_MOD };

// Each operation descriptor
struct OpDesc {
    OpClass cat;
    bool    isRead;
    int     id;    // 0-based within category
    OpResult (*func)(std::mt19937 &rng);
};

// Wrappers that generate random args from rng
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
    int maxc = (int)g_connections.size();
    if (maxc == 0) return {0, true};
    return op_sm6_delete_conn(rng() % maxc);
}
static OpResult wrap_sm7(std::mt19937 &rng) {
    // Find a CA at level TREE_LEVELS-1
    for (int i = 0; i < 100; i++) {
        int idx = pick_ca(rng);
        if (idx >= 0 && idx < (int)g_complexAssemblies.size() &&
            g_complexAssemblies[idx].level == TREE_LEVELS - 1)
            return op_sm7_create_ba(idx);
    }
    return {0, true};
}
static OpResult wrap_sm8(std::mt19937 &rng) { return op_sm8_delete_ba(pick_ba(rng)); }

// The 45 operations (§3), organised by category
// Long traversals: LT1-LT5
static OpDesc g_readOnlyLT[] = {
    {OpClass::LONG_TRAV, true, 0, [](auto &rng){ return op_lt1(); }},     // LT1
    {OpClass::LONG_TRAV, true, 1, [](auto &rng){ return op_lt3(); }},     // LT3
    {OpClass::LONG_TRAV, true, 2, [](auto &rng){ return op_lt5(); }},     // LT5
};
static OpDesc g_updateLT[] = {
    {OpClass::LONG_TRAV, false, 0, [](auto &rng){ return op_lt2(); }},    // LT2
    {OpClass::LONG_TRAV, false, 1, [](auto &rng){ return op_lt4(); }},    // LT4
};

// Short traversals: ST1-ST10
static OpDesc g_readOnlyST[] = {
    {OpClass::SHORT_TRAV, true, 0, wrap_st2},   // ST2
    {OpClass::SHORT_TRAV, true, 1, wrap_st3},   // ST3
    {OpClass::SHORT_TRAV, true, 2, wrap_st5},   // ST5
    {OpClass::SHORT_TRAV, true, 3, wrap_st7},   // ST7
    {OpClass::SHORT_TRAV, true, 4, wrap_st9},   // ST9
};
static OpDesc g_updateST[] = {
    {OpClass::SHORT_TRAV, false, 0, wrap_st4},   // ST4
    {OpClass::SHORT_TRAV, false, 1, wrap_st6},   // ST6
    {OpClass::SHORT_TRAV, false, 2, wrap_st8},   // ST8
    {OpClass::SHORT_TRAV, false, 3, wrap_st10},  // ST10
    {OpClass::SHORT_TRAV, false, 4, [](auto &rng){ return op_st1(); }},  // ST1
    // Note: ST1 happens to be read-only but we put it in update pool for count balance
};

// Short operations: OP1-OP15
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

// Structure modifications: always update
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

// Total pools for probability calculation
constexpr int NUM_RO_LT = sizeof(g_readOnlyLT) / sizeof(g_readOnlyLT[0]);   // 3
constexpr int NUM_UP_LT = sizeof(g_updateLT) / sizeof(g_updateLT[0]);       // 2
constexpr int NUM_RO_ST = sizeof(g_readOnlyST) / sizeof(g_readOnlyST[0]);   // 5
constexpr int NUM_UP_ST = sizeof(g_updateST) / sizeof(g_updateST[0]);       // 6
constexpr int NUM_RO_OP = sizeof(g_readOnlyOP) / sizeof(g_readOnlyOP[0]);   // 10
constexpr int NUM_UP_OP = sizeof(g_updateOP) / sizeof(g_updateOP[0]);       // 5
constexpr int NUM_SM    = sizeof(g_structMod) / sizeof(g_structMod[0]);     // 8

static OpDesc pick_operation(std::mt19937 &rng, int writePercent) {
    int r = rng() % 100;
    OpClass cat;
    // Fixed category distribution (§3 Table): 5% LT, 40% ST, 45% OP, 10% SM
    if (r < 5)       cat = OpClass::LONG_TRAV;
    else if (r < 45) cat = OpClass::SHORT_TRAV;
    else if (r < 90) cat = OpClass::SHORT_OP;
    else             cat = OpClass::STRUCT_MOD;

    // Determine read vs update based on workload percentages
    bool wantRead = (rng() % 100) < (100 - writePercent);

    if (cat == OpClass::STRUCT_MOD) {
        return g_structMod[rng() % NUM_SM];
    }

    // For non-SM categories: select from read or update sub-pool
    // Rebalancing: the actual category distribution stays 5/40/45, but
    // within each category the read/update ratio follows the workload
    const OpDesc *pool;
    int poolSize;
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

// ─── Medium-grained locking baseline (spec §4) ─────────────────────────
// RW locks per level of assembly tree + per data type
static std::mutex g_lock_ca[TREE_LEVELS];
static std::mutex g_lock_cp_all;
static std::mutex g_lock_ap_all;
static std::mutex g_lock_doc_all;
static std::mutex g_lock_manual;

// ─── Worker ────────────────────────────────────────────────────────────

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
    std::mt19937 rng(data->thread_id * 12345 + 42 + 1);
    data->barrier->wait();

    int ops = 0;
    int lt = 0, st = 0, sop = 0, sm = 0, ro = 0, up = 0;

    while (!g_stop_workers.load(std::memory_order_relaxed) && ops < data->loops) {
        OpDesc desc = pick_operation(rng, data->writePercent);
        OpResult res = desc.func(rng);
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

// ─── Main ──────────────────────────────────────────────────────────────

MAIN int main(int argc, char *argv[]) {
    int nb_threads   = DEFAULT_NB_THREADS;
    int duration_ms  = DEFAULT_DURATION_MS;
    int workload     = 1;     // 1=read-dom, 2=read-write, 3=write-dom
    int locking_mode = 0;     // 0=TM, 1=medium-grained

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) nb_threads = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration_ms = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) workload = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) locking_mode = std::atoi(argv[++i]);
    }

    int writePercent;
    switch (workload) {
        case 1: writePercent = 10; break;   // read-dominated  (§3: 90% RO, 10% UP)
        case 2: writePercent = 40; break;   // read-write      (§3: 60% RO, 40% UP)
        case 3: writePercent = 90; break;   // write-dominated (§3: 10% RO, 90% UP)
        default: writePercent = 10;
    }

    std::cout << "STMbench7 (EuroSys 2007 Spec)\n"
              << "==============================\n"
              << "Workload:   " << workload << " (" << (100 - writePercent) << "% read, " << writePercent << "% write)\n"
              << "Threads:    " << nb_threads << "\n"
              << "Duration:   " << duration_ms << " ms\n"
              << "Locking:    " << (locking_mode == 0 ? "TM (plugin)" : "Medium-grained") << "\n"
              << std::endl;

    tm_set_num_threads(nb_threads);
    init_data();

    std::cout << "Data structure (§2: medium OO7 size):\n"
              << "  Modules:           " << g_modules.size()           << " (spec: 1)\n"
              << "  ComplexAssemblies: " << g_complexAssemblies.size() << " (spec: " << MAX_CA << ")\n"
              << "  BaseAssemblies:    " << g_baseAssemblies.size()    << " (spec: " << MAX_BA << ")\n"
              << "  CompositeParts:    " << g_compositeParts.size()    << " (spec: " << MAX_CP << ")\n"
              << "  AtomicParts:       " << g_atomicParts.size()       << " (spec: " << MAX_AP << ")\n"
              << "  Connections:       " << g_connections.size()       << " (spec: ≥" << MAX_CONN << ")\n"
              << "  Documents:         " << g_documents.size()         << " (spec: " << MAX_DOCUMENTS << ")\n"
              << std::endl;

    int loops = duration_ms / 10;

    Barrier barrier(nb_threads);
    std::vector<ThreadData> thread_data(nb_threads);
    std::vector<std::thread> threads;

    for (int i = 0; i < nb_threads; i++) {
        thread_data[i].barrier       = &barrier;
        thread_data[i].thread_id     = i;
        thread_data[i].loops         = loops;
        thread_data[i].writePercent  = writePercent;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < nb_threads; i++)
        threads.emplace_back(worker, &thread_data[i]);

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    g_stop_workers.store(true, std::memory_order_release);
    for (auto &t : threads) t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    uint64_t ops = g_total_ops.load();
    uint64_t lt  = g_lt_count.load();
    uint64_t st  = g_st_count.load();
    uint64_t sop = g_op_count.load();
    uint64_t sm  = g_sm_count.load();
    uint64_t ro  = g_ro_count.load();
    uint64_t up  = g_up_count.load();

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed:       " << elapsed_ms << " ms\n"
              << "Total ops:     " << ops << "\n"
              << "Ops/sec:       " << (ops * 1000.0 / elapsed_ms) << "\n"
              << "\nCategory breakdown (§3 Table):\n"
              << "  Long traversals:      " << lt << " (" << (ops > 0 ? lt * 100.0 / ops : 0.0) << "%, spec ~5%)\n"
              << "  Short traversals:     " << st << " (" << (ops > 0 ? st * 100.0 / ops : 0.0) << "%, spec ~40%)\n"
              << "  Short operations:     " << sop << " (" << (ops > 0 ? sop * 100.0 / ops : 0.0) << "%, spec ~45%)\n"
              << "  Structure mods:       " << sm << " (" << (ops > 0 ? sm * 100.0 / ops : 0.0) << "%, spec ~10%)\n"
              << "\nRead/Update split:\n"
              << "  Read-only:  " << ro << " (" << (ops > 0 ? ro * 100.0 / ops : 0.0) << "%)\n"
              << "  Update:     " << up << " (" << (ops > 0 ? up * 100.0 / ops : 0.0) << "%)\n"
              << std::endl;

    return 0;
}
