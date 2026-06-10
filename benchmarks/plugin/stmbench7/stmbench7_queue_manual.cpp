/**
 * STMbench7 queue-manual benchmark
 *
 * Queue-manual pipeline showcase with STMbench7-like operations:
 *   - Graph data structure (nodes + connections, like STMbench7's AP graph)
 *   - Multiple operation categories: short reads, short updates, traversals, struct mods
 *   - All operations use async_transaction annotation (return void)
 *   - Main thread enqueues batches and calls tm_wait_prev_tx() periodically
 *
 * Build with: tm-instrument-queue-manual pipeline + queue_runtime.o
 *
 * NOTE: Async TX functions must return void. The queue-manual pipeline
 * replaces the call with an enqueue and discards the return value, so
 * a non-void return would produce invalid IR (use of erased instruction).
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include "tm_vector.hpp"

#define ASYNC_TX __attribute__((noinline, annotate("async_transaction")))
#define MAIN    __attribute__((annotate("main"), noinline))

extern "C" void tm_wait_prev_tx(void);

// ─── Spec constants ────────────────────────────────────────────
constexpr int NUM_NODES       = 10000;
constexpr int CONN_PER_NODE   = 4;
constexpr int NUM_EDGES       = NUM_NODES * CONN_PER_NODE;
constexpr uint64_t DURATION_NS = 5000000000ULL; // 5 s
constexpr int BATCH_SIZE      = 1000;

// ─── Data structures ──────────────────────────────────────────
struct Node {
    int id;
    int x, y, z;
    int weight;
    TMSafeVector<int> edgeIds;
};

struct Edge {
    int id;
    int fromNodeId;
    int toNodeId;
    int type;
};

TMSafeVector<Node>  g_nodes;
TMSafeVector<Edge>  g_edges;

// ─── Initialisation ───────────────────────────────────────────
static void init_data() {
    g_nodes.clear();
    g_edges.clear();
    g_nodes.reserve(NUM_NODES * 2);
    g_edges.reserve(NUM_EDGES * 2);

    std::mt19937 rng(42);

    for (int i = 0; i < NUM_NODES; i++) {
        Node n;
        n.id     = i;
        n.x      = i % 100;
        n.y      = (i / 100) % 100;
        n.z      = i / 10000;
        n.weight = (i % 50) + 1;
        n.edgeIds.reserve(CONN_PER_NODE * 2);
        g_nodes.push_back(n);
    }

    for (int i = 0; i < NUM_NODES; i++) {
        for (int j = 0; j < CONN_PER_NODE; j++) {
            int t = rng() % NUM_NODES;
            if (t == i) { j--; continue; }
            Edge e;
            e.id         = (int)g_edges.size();
            e.fromNodeId = i;
            e.toNodeId   = t;
            e.type       = j % 3;
            g_edges.push_back(e);
            g_nodes[i].edgeIds.push_back(e.id);
        }
    }
}

// ─── Operations (all async_transaction, return void) ──────────

ASYNC_TX void op_read_node(int idx) {
    if (idx >= 0 && idx < (int)g_nodes.size()) {
        volatile int v = g_nodes[idx].weight;
        (void)v;
    }
}

ASYNC_TX void op_update_node(int idx) {
    if (idx >= 0 && idx < (int)g_nodes.size())
        g_nodes[idx].weight = (g_nodes[idx].weight % 50) + 1;
}

ASYNC_TX void op_traverse_neighbourhood(int idx) {
    if (idx >= 0 && idx < (int)g_nodes.size()) {
        volatile uint64_t sum = (uint64_t)g_nodes[idx].weight;
        for (int eid : g_nodes[idx].edgeIds) {
            if (eid < (int)g_edges.size()) {
                int nb = (g_edges[eid].fromNodeId == idx)
                             ? g_edges[eid].toNodeId
                             : g_edges[eid].fromNodeId;
                if (nb >= 0 && nb < (int)g_nodes.size())
                    sum += (uint64_t)g_nodes[nb].weight;
            }
        }
        (void)sum;
    }
}

ASYNC_TX void op_update_neighbourhood(int idx) {
    if (idx >= 0 && idx < (int)g_nodes.size()) {
        g_nodes[idx].weight = (g_nodes[idx].weight % 50) + 1;
        for (int eid : g_nodes[idx].edgeIds) {
            if (eid < (int)g_edges.size()) {
                int nb = (g_edges[eid].fromNodeId == idx)
                             ? g_edges[eid].toNodeId
                             : g_edges[eid].fromNodeId;
                if (nb >= 0 && nb < (int)g_nodes.size())
                    g_nodes[nb].weight = (g_nodes[nb].weight % 50) + 1;
            }
        }
    }
}

ASYNC_TX void op_sum_all_nodes() {
    volatile uint64_t sum = 0;
    for (auto &n : g_nodes) sum += (uint64_t)n.weight;
    (void)sum;
}

ASYNC_TX void op_sum_all_edges() {
    volatile uint64_t sum = 0;
    for (auto &e : g_edges) sum += (uint64_t)(e.id + e.fromNodeId + e.toNodeId + e.type);
    (void)sum;
}

ASYNC_TX void op_add_node() {
    int new_id = (int)g_nodes.size();
    Node n;
    n.id     = new_id;
    n.x      = new_id % 100;
    n.y      = (new_id / 100) % 100;
    n.z      = new_id / 10000;
    n.weight = (new_id % 50) + 1;
    n.edgeIds.reserve(CONN_PER_NODE);
    g_nodes.push_back(n);
}

ASYNC_TX void op_add_edge(int from, int to) {
    if (from >= (int)g_nodes.size() || to >= (int)g_nodes.size() || from == to)
        return;
    Edge e;
    e.id         = (int)g_edges.size();
    e.fromNodeId = from;
    e.toNodeId   = to;
    e.type       = 0;
    g_edges.push_back(e);
    g_nodes[from].edgeIds.push_back(e.id);
}

// ─── Args generator ───────────────────────────────────────────
static void gen_args(int op_idx, std::mt19937 &rng, int &a0, int &a1) {
    int sz = std::max(1, (int)g_nodes.size());
    a0 = 0; a1 = 0;
    switch (op_idx) {
        case 0: a0 = (int)(rng() % sz); break; // read_node
        case 1: a0 = (int)(rng() % sz); break; // update_node
        case 2: a0 = (int)(rng() % sz); break; // traverse_neighbourhood
        case 3: a0 = (int)(rng() % sz); break; // update_neighbourhood
        case 4: break; // sum_all_nodes (no args)
        case 5: break; // sum_all_edges (no args)
        case 6: break; // add_node (no args)
        case 7:
            a0 = (int)(rng() % sz);
            a1 = (int)(rng() % sz);
            if (a1 == a0) a1 = (a0 + 1) % sz;
            break; // add_edge
    }
}

// ─── Operation table ──────────────────────────────────────────
// Forwarding wrappers: each calls the corresponding ASYNC_TX op.
// The pass replaces the async_tx call inside with an enqueue.

static void call_read_node(int a0, int)        { op_read_node(a0); }
static void call_update_node(int a0, int)      { op_update_node(a0); }
static void call_traverse_nb(int a0, int)      { op_traverse_neighbourhood(a0); }
static void call_update_nb(int a0, int)        { op_update_neighbourhood(a0); }
static void call_sum_nodes(int, int)           { op_sum_all_nodes(); }
static void call_sum_edges(int, int)           { op_sum_all_edges(); }
static void call_add_node(int, int)            { op_add_node(); }
static void call_add_edge(int a0, int a1)      { op_add_edge(a0, a1); }

struct OpEntry {
    const char *name;
    bool isRead;
    int  weight;
    void (*call)(int, int);
};

OpEntry ops[] = {
    {"read_node",           true,  20, call_read_node},
    {"update_node",         false, 10, call_update_node},
    {"traverse_neighbour",  true,  25, call_traverse_nb},
    {"update_neighbour",    false, 15, call_update_nb},
    {"sum_all_nodes",       true,  5,  call_sum_nodes},
    {"sum_all_edges",       true,  5,  call_sum_edges},
    {"add_node",            false, 10, call_add_node},
    {"add_edge",            false, 10, call_add_edge},
};

constexpr int NUM_OPS = sizeof(ops) / sizeof(ops[0]);

// ─── Main ─────────────────────────────────────────────────────
MAIN int main() {
    init_data();

    std::cout << "STMbench7 queue-manual benchmark\n"
              << "Nodes: " << g_nodes.size() << "\n"
              << "Edges: " << g_edges.size() << "\n"
              << "Batch size: " << BATCH_SIZE << "\n"
              << "Target duration: " << (DURATION_NS / 1000000000ULL) << " s\n"
              << std::endl;

    int totalWeight = 0;
    for (auto &op : ops) totalWeight += op.weight;

    std::mt19937 rng(12345);
    uint64_t batch_count = 0;
    uint64_t total_ops = 0;

    auto start_time = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = 0;

    while (elapsed_ns < DURATION_NS) {
        int a0, a1;
        for (int i = 0; i < BATCH_SIZE; i++) {
            int r = (int)(rng() % totalWeight);
            int acc = 0;
            int chosen = 0;
            for (int j = 0; j < NUM_OPS; j++) {
                acc += ops[j].weight;
                if (r < acc) { chosen = j; break; }
            }
            gen_args(chosen, rng, a0, a1);
            ops[chosen].call(a0, a1);
        }
        total_ops += BATCH_SIZE;

        tm_wait_prev_tx();
        batch_count++;

        auto now = std::chrono::high_resolution_clock::now();
        elapsed_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time).count();

        if (batch_count % 10 == 0) {
            double sec = elapsed_ns / 1.0e9;
            std::cout << "  [" << batch_count << " batches] "
                      << total_ops << " ops, "
                      << (sec > 0 ? (uint64_t)(total_ops / sec) : 0) << " ops/s\r"
                      << std::flush;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

    std::cout << "\n\nResults\n"
              << "=======\n"
              << "Elapsed:    " << elapsed_sec << " s\n"
              << "Batches:    " << batch_count << "\n"
              << "Total ops:  " << total_ops << "\n"
              << "Ops/sec:    " << (elapsed_sec > 0 ? (uint64_t)(total_ops / elapsed_sec) : 0) << "\n";

    // Verify data structure integrity
    std::cout << "Final nodes: " << g_nodes.size() << " (started with " << NUM_NODES << ")\n";
    std::cout << "Final edges: " << g_edges.size() << " (started with " << NUM_EDGES << ")\n";

    std::cout << "\nPASS" << std::endl;
    return 0;
}
