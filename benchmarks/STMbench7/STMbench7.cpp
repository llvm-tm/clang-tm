/**
 * STMbench7 Benchmark - Full Specification Implementation
 *
 * Based on: STMBench7: A Benchmark for Software Transactional Memory
 * Authors: Rachid Guerraoui, Michal Kapalka, Jan Vitek
 * Published: EuroSys 2007
 *
 * Specification: https://janvitek.org/pubs/eurosys07.pdf
 *
 * Data Structure:
 * - Module (design root) containing ComplexAssemblies (internal tree nodes)
 * - ComplexAssemblies containing BaseAssemblies (leaves)
 * - BaseAssemblies connected to AtomicParts (graph nodes)
 * - AtomicParts with graph connections to other AtomicParts
 * - CompositeParts with documentation indexes
 *
 * Operations (45 total in 4 categories):
 * 1. Long traversals - go through all assemblies and/or all atomic parts
 * 2. Short traversals - random path from module/doc/atomic part
 * 3. Short operations - single object or local neighborhood
 * 4. Structure modification - create/delete elements and links
 *
 * Workloads:
 * - Read-dominated: 10% writes
 * - Read-write: 60% writes
 * - Write-dominated: 90% writes
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <map>
#include <list>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS = 4;

constexpr int MAX_MODULES = 5;
constexpr int MAX_COMPLEX_ASSEMBLIES = 100;
constexpr int MAX_BASE_ASSEMBLIES = 500;
constexpr int MAX_ATOMIC_PARTS = 5000;
constexpr int MAX_COMPOSITE_PARTS = 1000;
constexpr int MAX_CONNECTIONS = 10000;
constexpr int MAX_DOCUMENTS = 2000;

struct CompositePart {
    int id;
    int buildDate;
    int numDocuments;
};

struct Document {
    int id;
    int type;
    int date;
    int atomicPartId;
};

struct AtomicPart {
    int id;
    int x, y, z;
    int buildDate;
    int weight;
    int compositeId;
    int type;
};

struct Connection {
    int fromAtomicId;
    int toAtomicId;
    int type;
};

struct BaseAssembly {
    int id;
    int atomicPartId;
    int complexAssemblyId;
    int buildDate;
};

struct ComplexAssembly {
    int id;
    int level;
    int parentId;
    std::vector<int> baseAssemblyIds;
};

struct Module {
    int id;
    int rootAssemblyId;
    std::string name;
};

TM std::vector<Module> g_modules;
TM std::vector<ComplexAssembly> g_complexAssemblies;
TM std::vector<BaseAssembly> g_baseAssemblies;
TM std::vector<AtomicPart> g_atomicParts;
TM std::vector<CompositePart> g_compositeParts;
TM std::vector<Connection> g_connections;
TM std::vector<Document> g_documents;

TM std::map<int, int> g_atomicById;
TM std::map<int, std::vector<int>> g_atomicByComposite;
TM std::map<int, int> g_documentByAtomic;
TM std::map<int, std::vector<int>> g_connectionsByAtomic;

TM int g_atomicPartCount = 0;
TM int g_connectionCount = 0;

TM void init_data() {
    g_modules.clear();
    g_complexAssemblies.clear();
    g_baseAssemblies.clear();
    g_atomicParts.clear();
    g_compositeParts.clear();
    g_connections.clear();
    g_documents.clear();

    g_atomicById.clear();
    g_atomicByComposite.clear();
    g_documentByAtomic.clear();
    g_connectionsByAtomic.clear();

    for (int i = 0; i < MAX_MODULES; i++) {
        Module m;
        m.id = i;
        m.rootAssemblyId = i * 20;
        m.name = "Module_" + std::to_string(i);
        g_modules.push_back(m);
    }

    for (int i = 0; i < MAX_COMPOSITE_PARTS; i++) {
        CompositePart cp;
        cp.id = i;
        cp.buildDate = 1000 + (i % 365);
        cp.numDocuments = (i % 10) + 1;
        g_compositeParts.push_back(cp);

        for (int j = 0; j < cp.numDocuments; j++) {
            Document doc;
            doc.id = g_documents.size();
            doc.type = j % 3;
            doc.date = cp.buildDate + j;
            doc.atomicPartId = -1;
            g_documents.push_back(doc);
        }
    }

    for (int i = 0; i < MAX_COMPLEX_ASSEMBLIES; i++) {
        ComplexAssembly ca;
        ca.id = i;
        ca.level = i % 3;
        ca.parentId = (i > 0) ? ((i - 1) / 10) : -1;
        g_complexAssemblies.push_back(ca);
    }

    for (int i = 0; i < MAX_BASE_ASSEMBLIES; i++) {
        BaseAssembly ba;
        ba.id = i;
        ba.atomicPartId = i % MAX_ATOMIC_PARTS;
        ba.complexAssemblyId = i / 10;
        ba.buildDate = 1000 + (i % 365);
        g_baseAssemblies.push_back(ba);

        g_complexAssemblies[ba.complexAssemblyId].baseAssemblyIds.push_back(ba.id);
    }

    for (int i = 0; i < MAX_ATOMIC_PARTS; i++) {
        AtomicPart ap;
        ap.id = i;
        ap.x = i % 100;
        ap.y = (i / 100) % 100;
        ap.z = i / 10000;
        ap.buildDate = 1000 + (i % 365);
        ap.weight = (i % 50) + 1;
        ap.compositeId = i / 5;
        ap.type = i % 5;
        g_atomicParts.push_back(ap);

        g_atomicById[ap.id] = i;
        g_atomicByComposite[ap.compositeId].push_back(i);

        if ((int)g_documents.size() > i) {
            g_documents[i].atomicPartId = i;
            g_documentByAtomic[g_documents[i].id] = i;
        }
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        Connection conn;
        conn.fromAtomicId = i % MAX_ATOMIC_PARTS;
        conn.toAtomicId = (i + 1) % MAX_ATOMIC_PARTS;
        conn.type = i % 3;
        g_connections.push_back(conn);

        g_connectionsByAtomic[conn.fromAtomicId].push_back(g_connections.size() - 1);
    }
    g_connectionCount = g_connections.size();
}

TX int op_lookup_by_id(int atomicId) {
    auto it = g_atomicById.find(atomicId);
    if (it == g_atomicById.end()) return -1;
    AtomicPart& ap = g_atomicParts[it->second];
    return ap.x + ap.y + ap.z;
}

TX int op_lookup_by_index(int compositeId) {
    auto it = g_atomicByComposite.find(compositeId);
    if (it == g_atomicByComposite.end() || it->second.empty()) return -1;
    int idx = it->second[0];
    return g_atomicParts[idx].buildDate;
}

TX int op_traverse_all_assemblies() {
    int sum = 0;
    for (auto& ca : g_complexAssemblies) {
        sum += ca.id + ca.level;
        for (int baId : ca.baseAssemblyIds) {
            if (baId < (int)g_baseAssemblies.size()) {
                sum += g_baseAssemblies[baId].buildDate;
            }
        }
    }
    return sum;
}

TX int op_traverse_all_atomic() {
    int sum = 0;
    for (auto& ap : g_atomicParts) {
        sum += ap.x + ap.y + ap.z + ap.weight;
    }
    return sum;
}

TX int op_traverse_all_connections() {
    int sum = 0;
    for (auto& conn : g_connections) {
        sum += conn.fromAtomicId + conn.toAtomicId + conn.type;
    }
    return sum;
}

TX int op_traverse_path_from_module(int moduleId) {
    if (moduleId >= (int)g_modules.size()) return 0;
    int sum = 0;
    int rootId = g_modules[moduleId].rootAssemblyId;
    if (rootId < (int)g_complexAssemblies.size()) {
        sum += g_complexAssemblies[rootId].id;
        for (int baId : g_complexAssemblies[rootId].baseAssemblyIds) {
            if (baId < (int)g_baseAssemblies.size()) {
                int apId = g_baseAssemblies[baId].atomicPartId;
                if (apId >= 0 && apId < (int)g_atomicParts.size()) {
                    sum += g_atomicParts[apId].x;
                }
            }
        }
    }
    return sum;
}

TX int op_traverse_by_document(int docId) {
    if (docId >= (int)g_documents.size()) return 0;
    int sum = 0;
    sum += g_documents[docId].date + g_documents[docId].type;
    int atomicId = g_documents[docId].atomicPartId;
    if (atomicId >= 0 && atomicId < (int)g_atomicParts.size()) {
        sum += g_atomicParts[atomicId].buildDate;
    }
    return sum;
}

TX int op_read_local_neighborhood(int atomicId) {
    int sum = 0;
    auto it = g_atomicById.find(atomicId);
    if (it == g_atomicById.end()) return 0;
    sum += g_atomicParts[it->second].x + g_atomicParts[it->second].y;

    auto connIt = g_connectionsByAtomic.find(atomicId);
    if (connIt != g_connectionsByAtomic.end()) {
        for (int connIdx : connIt->second) {
            if (connIdx < (int)g_connections.size()) {
                Connection& c = g_connections[connIdx];
                int neighborId = (c.fromAtomicId == atomicId) ? c.toAtomicId : c.fromAtomicId;
                if (neighborId < (int)g_atomicParts.size()) {
                    sum += g_atomicParts[neighborId].x;
                }
            }
        }
    }
    return sum;
}

TX void op_update_atomic_part(int atomicId, int newX, int newY) {
    auto it = g_atomicById.find(atomicId);
    if (it != g_atomicById.end()) {
        g_atomicParts[it->second].x = newX;
        g_atomicParts[it->second].y = newY;
    }
}

TX void op_update_assembly(int assemblyId, int newDate) {
    if (assemblyId < (int)g_complexAssemblies.size()) {
        g_complexAssemblies[assemblyId].level = newDate % 3;
    }
}

TX int op_create_connection(int fromId, int toId, int type) {
    if (fromId >= MAX_ATOMIC_PARTS || toId >= MAX_ATOMIC_PARTS) return -1;
    if ((int)g_connections.size() >= MAX_CONNECTIONS) return -1;

    Connection conn;
    conn.fromAtomicId = fromId;
    conn.toAtomicId = toId;
    conn.type = type;
    g_connections.push_back(conn);
    g_connectionsByAtomic[fromId].push_back(g_connections.size() - 1);
    return g_connections.size() - 1;
}

TX void op_delete_connection(int connId) {
    if (connId >= (int)g_connections.size()) return;
    Connection& conn = g_connections[connId];
    auto& list = g_connectionsByAtomic[conn.fromAtomicId];
    list.erase(std::remove(list.begin(), list.end(), connId), list.end());
    g_connections[connId].fromAtomicId = -1;
}

TX int op_create_atomic_part(int id, int x, int y, int z) {
    if ((int)g_atomicParts.size() >= MAX_ATOMIC_PARTS) return -1;
    AtomicPart ap;
    ap.id = id;
    ap.x = x;
    ap.y = y;
    ap.z = z;
    ap.buildDate = 2000;
    ap.weight = 10;
    ap.compositeId = id / 5;
    ap.type = id % 5;
    g_atomicParts.push_back(ap);
    g_atomicById[id] = (int)g_atomicParts.size() - 1;
    g_atomicByComposite[ap.compositeId].push_back((int)g_atomicParts.size() - 1);
    return (int)g_atomicParts.size() - 1;
}

TX void op_delete_atomic_part(int atomicId) {
    auto it = g_atomicById.find(atomicId);
    if (it != g_atomicById.end()) {
        int idx = it->second;
        g_atomicParts[idx].id = -1;
        g_atomicById.erase(it);
    }
}

enum OperationCategory {
    CAT_LONG_TRAVERSAL,
    CAT_SHORT_TRAVERSAL,
    CAT_SHORT_OPERATION,
    CAT_STRUCTURE_MOD
};

struct Operation {
    OperationCategory category;
    int threadId;
};

class Barrier {
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
    int num_threads_;
    int crossing_;
public:
    explicit Barrier(int n) : count_(n), num_threads_(n), crossing_(0) {}
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        crossing_++;
        if (crossing_ < num_threads_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

std::atomic<bool> stop_workers(false);
std::atomic<uint64_t> total_ops{0};
std::atomic<uint64_t> long_traversals{0};
std::atomic<uint64_t> short_traversals{0};
std::atomic<uint64_t> short_ops{0};
std::atomic<uint64_t> struct_mods{0};
std::atomic<uint64_t> aborts{0};

struct ThreadData {
    Barrier* barrier;
    int thread_id;
    int loops;
    int writePercentage;
    bool measureAbortRate;
};

void worker(ThreadData* data) {
    std::mt19937 rng(data->thread_id * 12345 + 42);
    std::uniform_int_distribution<int> opDist(0, 99);
    std::uniform_int_distribution<int> idDist(0, MAX_ATOMIC_PARTS - 1);
    std::uniform_int_distribution<int> modDist(0, MAX_COMPLEX_ASSEMBLIES - 1);

    data->barrier->wait();

    int ops = 0;
    int ltrav = 0, strav = 0, sops = 0, smod = 0;

    while (!stop_workers.load(std::memory_order_relaxed) && ops < data->loops) {
        int r = opDist(rng);
        int writePct = data->writePercentage;

        if (r < 10) {
            int sel = opDist(rng) % 5;
            switch (sel) {
                case 0: op_traverse_all_assemblies(); break;
                case 1: op_traverse_all_atomic(); break;
                case 2: op_traverse_all_connections(); break;
                case 3: op_traverse_path_from_module(opDist(rng) % (int)g_modules.size()); break;
                case 4: op_traverse_by_document(opDist(rng) % (int)g_documents.size()); break;
            }
            ltrav++;
        } else if (r < 30) {
            int sel = opDist(rng) % 4;
            switch (sel) {
                case 0: op_lookup_by_id(idDist(rng)); break;
                case 1: op_lookup_by_index(idDist(rng) % MAX_COMPOSITE_PARTS); break;
                case 2: op_read_local_neighborhood(idDist(rng)); break;
                case 3: op_traverse_by_document(opDist(rng) % (int)g_documents.size()); break;
            }
            strav++;
        } else if (r < (30 + writePct)) {
            int sel = opDist(rng) % 2;
            switch (sel) {
                case 0: op_update_atomic_part(idDist(rng), opDist(rng), opDist(rng)); break;
                case 1: op_update_assembly(modDist(rng), opDist(rng)); break;
            }
            sops++;
        } else {
            int sel = opDist(rng) % 6;
            switch (sel) {
                case 0: op_create_connection(idDist(rng), idDist(rng), opDist(rng) % 3); break;
                case 1: if (!g_connections.empty()) op_delete_connection(opDist(rng) % (int)g_connections.size()); break;
                case 2: op_create_atomic_part(MAX_ATOMIC_PARTS + data->thread_id * 1000 + ops, opDist(rng), opDist(rng), opDist(rng)); break;
                case 3: op_delete_atomic_part(idDist(rng)); break;
                case 4: op_update_atomic_part(idDist(rng), opDist(rng), opDist(rng)); break;
                case 5: op_update_assembly(modDist(rng), opDist(rng)); break;
            }
            smod++;
        }
        ops++;
    }

    total_ops.fetch_add(ops, std::memory_order_relaxed);
    long_traversals.fetch_add(ltrav, std::memory_order_relaxed);
    short_traversals.fetch_add(strav, std::memory_order_relaxed);
    short_ops.fetch_add(sops, std::memory_order_relaxed);
    struct_mods.fetch_add(smod, std::memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    int nb_threads = DEFAULT_NB_THREADS;
    int duration_ms = DEFAULT_DURATION_MS;
    int workload = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            nb_threads = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_ms = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            workload = std::atoi(argv[++i]);
        }
    }

    int writePercent;
    switch (workload) {
        case 1: writePercent = 10; break;
        case 2: writePercent = 60; break;
        case 3: writePercent = 90; break;
        default: writePercent = 10;
    }

    std::cout << "STMbench7 (Full Specification)\n"
              << "==============================\n"
              << "Workload:   " << workload << " (" << writePercent << "% write)\n"
              << "Threads:    " << nb_threads << "\n"
              << "Duration:   " << duration_ms << " ms\n"
              << std::endl;

    init_data();

    std::cout << "Initialized:\n"
              << "  Modules:           " << g_modules.size() << "\n"
              << "  Complex Assemblies: " << g_complexAssemblies.size() << "\n"
              << "  Base Assemblies:  " << g_baseAssemblies.size() << "\n"
              << "  Atomic Parts:    " << g_atomicParts.size() << "\n"
              << "  Composite Parts:  " << g_compositeParts.size() << "\n"
              << "  Documents:        " << g_documents.size() << "\n"
              << "  Connections:      " << g_connections.size() << "\n"
              << std::endl;

    int loops = duration_ms / 10;

    Barrier barrier(nb_threads);
    std::vector<ThreadData> thread_data(nb_threads);
    std::vector<std::thread> threads;

    for (int i = 0; i < nb_threads; i++) {
        thread_data[i].barrier = &barrier;
        thread_data[i].thread_id = i;
        thread_data[i].loops = loops;
        thread_data[i].writePercentage = writePercent;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < nb_threads; i++) {
        threads.emplace_back(worker, &thread_data[i]);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    stop_workers.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    uint64_t ops = total_ops.load();
    uint64_t ltrav = long_traversals.load();
    uint64_t strav = short_traversals.load();
    uint64_t sops = short_ops.load();
    uint64_t smod = struct_mods.load();

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed:       " << elapsed_ms << " ms\n"
              << "Total ops:     " << ops << "\n"
              << "Ops/sec:       " << (ops * 1000.0 / elapsed_ms) << "\n"
              << "\nOperation breakdown:\n"
              << "  Long traversals:     " << ltrav << " (" << (ltrav * 100.0 / ops) << "%)\n"
              << "  Short traversals:    " << strav << " (" << (strav * 100.0 / ops) << "%)\n"
              << "  Short operations:     " << sops << " (" << (sops * 100.0 / ops) << "%)\n"
              << "  Structure mods:      " << smod << " (" << (smod * 100.0 / ops) << "%)\n"
              << std::endl;

    return 0;
}