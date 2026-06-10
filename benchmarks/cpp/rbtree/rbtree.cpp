// RBtree benchmark for TM — stresses speculative malloc / deferred free
// Uses tm_malloc for node allocation inside transactions.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "expli_tm_api/tm_api.hpp"

// ── Configuration ─────────────────────────────────────────────
static int g_duration_ms = 2000;
static int g_nthreads = 4;
static int g_initial_keys = 256;
static std::atomic<bool> g_stop{false};

// ── BST node (no balancing — simpler, correct by construction) ─
struct BSTNode {
    expli::TM<int64_t> key;
    expli::TM<uint64_t> left;
    expli::TM<uint64_t> right;
};

static BSTNode *g_root = nullptr;

static BSTNode *alloc_node(int64_t key) {
    auto *n = (BSTNode *)tm_malloc(sizeof(BSTNode));
    if (!n) { fprintf(stderr, "OOM\n"); exit(1); }
    n->key.poke(key);
    n->left.poke(0ULL);
    n->right.poke(0ULL);
    return n;
}

// ── Serial helpers (poke) for single-threaded init ────────────
static bool insert_serial(BSTNode **root, BSTNode *z) {
    int64_t key = z->key.peek();
    if (!*root) { *root = z; return true; }
    BSTNode *cur = *root;
    for (;;) {
        int64_t k = cur->key.peek();
        if (key == k) { tm_free(z); return false; }
        if (key < k) {
            if (!cur->left.peek()) { cur->left.poke(reinterpret_cast<uint64_t>(z)); return true; }
            cur = reinterpret_cast<BSTNode*>(cur->left.peek());
        } else {
            if (!cur->right.peek()) { cur->right.poke(reinterpret_cast<uint64_t>(z)); return true; }
            cur = reinterpret_cast<BSTNode*>(cur->right.peek());
        }
    }
}

static BSTNode *search_serial(BSTNode *root, int64_t key) {
    while (root) {
        int64_t k = root->key.peek();
        if (key == k) return root;
        root = reinterpret_cast<BSTNode*>((key < k) ? root->left.peek() : root->right.peek());
    }
    return nullptr;
}

static bool delete_serial(BSTNode **root, int64_t key) {
    BSTNode *parent = nullptr;
    BSTNode *cur = *root;
    bool is_left = false;
    while (cur) {
        int64_t k = cur->key.peek();
        if (key == k) break;
        parent = cur;
        if (key < k) { is_left = true;  cur = reinterpret_cast<BSTNode*>(cur->left.peek()); }
        else         { is_left = false; cur = reinterpret_cast<BSTNode*>(cur->right.peek()); }
    }
    if (!cur) return false;

    BSTNode *l = reinterpret_cast<BSTNode*>(cur->left.peek());
    BSTNode *r = reinterpret_cast<BSTNode*>(cur->right.peek());
    BSTNode *child = nullptr;
    if (!l) child = r;
    else if (!r) child = l;
    else {
        BSTNode *succ_p = cur;
        BSTNode *succ = r;
        while (succ->left.peek()) { succ_p = succ; succ = reinterpret_cast<BSTNode*>(succ->left.peek()); }
        BSTNode *sc = reinterpret_cast<BSTNode*>(succ->right.peek());
        if (succ_p == cur) {
            succ->right.poke(reinterpret_cast<uint64_t>(sc));
        } else {
            succ_p->left.poke(reinterpret_cast<uint64_t>(sc));
            succ->right.poke(reinterpret_cast<uint64_t>(r));
        }
        succ->left.poke(reinterpret_cast<uint64_t>(l));
        if (!parent) *root = succ;
        else if (is_left) parent->left.poke(reinterpret_cast<uint64_t>(succ));
        else parent->right.poke(reinterpret_cast<uint64_t>(succ));
        tm_free(cur);
        return true;
    }

    if (!parent) *root = child;
    else if (is_left) parent->left.poke(reinterpret_cast<uint64_t>(child));
    else parent->right.poke(reinterpret_cast<uint64_t>(child));
    tm_free(cur);
    return true;
}

// ── TM-based helpers (read/write via TM) ──────────────────────
static BSTNode *get_left(BSTNode *n) {
    if (!n) return nullptr;
    return reinterpret_cast<BSTNode*>(n->left.read());
}
static BSTNode *get_right(BSTNode *n) {
    if (!n) return nullptr;
    return reinterpret_cast<BSTNode*>(n->right.read());
}
static void set_left(BSTNode *n, BSTNode *v) {
    n->left.write(reinterpret_cast<uint64_t>(v));
}
static void set_right(BSTNode *n, BSTNode *v) {
    n->right.write(reinterpret_cast<uint64_t>(v));
}

static bool tm_insert(BSTNode **root, BSTNode *z) {
    int64_t key = z->key.peek();
    if (!*root) { *root = z; return true; }
    BSTNode *cur = *root;
    for (;;) {
        int64_t k = cur->key.read();
        if (key == k) { tm_free(z); return false; }
        if (key < k) {
            BSTNode *nxt = get_left(cur);
            if (!nxt) { set_left(cur, z); return true; }
            cur = nxt;
        } else {
            BSTNode *nxt = get_right(cur);
            if (!nxt) { set_right(cur, z); return true; }
            cur = nxt;
        }
    }
}

static BSTNode *tm_search(BSTNode *root, int64_t key) {
    while (root) {
        int64_t k = root->key.read();
        if (key == k) return root;
        root = reinterpret_cast<BSTNode*>((key < k) ? root->left.read() : root->right.read());
    }
    return nullptr;
}

static bool tm_delete(BSTNode **root, int64_t key) {
    BSTNode *parent = nullptr;
    BSTNode *cur = *root;
    bool is_left = false;
    while (cur) {
        int64_t k = cur->key.read();
        if (key == k) break;
        parent = cur;
        if (key < k) { is_left = true;  cur = get_left(cur); }
        else         { is_left = false; cur = get_right(cur); }
    }
    if (!cur) return false;

    BSTNode *l = get_left(cur);
    BSTNode *r = get_right(cur);
    BSTNode *child = nullptr;
    if (!l) child = r;
    else if (!r) child = l;
    else {
        BSTNode *succ_p = cur;
        BSTNode *succ = r;
        while (get_left(succ)) { succ_p = succ; succ = get_left(succ); }
        BSTNode *sc = get_right(succ);
        if (succ_p == cur) {
            set_right(succ, sc);
        } else {
            set_left(succ_p, sc);
            set_right(succ, r);
        }
        set_left(succ, l);
        if (!parent) *root = succ;
        else if (is_left) set_left(parent, succ);
        else set_right(parent, succ);
        tm_free(cur);
        return true;
    }

    if (!parent) *root = child;
    else if (is_left) set_left(parent, child);
    else set_right(parent, child);
    tm_free(cur);
    return true;
}

// ── Validation (uses peek — non-transactional) ────────────────
static int tree_count(BSTNode *root) {
    if (!root) return 0;
    return 1 + tree_count((BSTNode*)root->left.peek()) + tree_count((BSTNode*)root->right.peek());
}

static bool tree_validate(BSTNode *root, int64_t lo, int64_t hi) {
    if (!root) return true;
    int64_t k = root->key.peek();
    if (k < lo || k > hi) {
        fprintf(stderr, "BST violation: key %lld out of range [%lld,%lld]\n",
                (long long)k, (long long)lo, (long long)hi);
        return false;
    }
    BSTNode *l = reinterpret_cast<BSTNode*>(root->left.peek());
    BSTNode *r = reinterpret_cast<BSTNode*>(root->right.peek());
    bool ok = true;
    if (l && l->key.peek() >= k) {
        fprintf(stderr, "BST violation: left child key %lld >= parent %lld\n",
                (long long)l->key.peek(), (long long)k);
        ok = false;
    }
    if (r && r->key.peek() <= k) {
        fprintf(stderr, "BST violation: right child key %lld <= parent %lld\n",
                (long long)r->key.peek(), (long long)k);
        ok = false;
    }
    return ok & tree_validate(l, lo, k-1) & tree_validate(r, k+1, hi);
}

// ── Worker thread ─────────────────────────────────────────────
struct ThreadData {
    int tid;
    int ops{0};
};

static void worker(ThreadData &d) {
    expli::TM<int>::thread_init();

    while (!g_stop.load()) {
        int key = rand() % (g_initial_keys * 4);
        int op = rand() % 10;
        if (op < 6) {          // 60% insert
            BSTNode *z = alloc_node(key);
            expli::TM<int>::transaction([&]() {
                if (!tm_insert(&g_root, z)) {
                    tm_free(z);
                }
            });
        } else if (op < 8) {   // 20% lookup
            expli::TM<int>::transaction([&]() {
                tm_search(g_root, key);
            });
        } else {               // 20% delete
            expli::TM<int>::transaction([&]() {
                tm_delete(&g_root, key);
            });
        }
        d.ops++;
    }

    expli::TM<int>::thread_exit();
}

int main(int argc, char **argv) {
    if (argc > 1) g_duration_ms = atoi(argv[1]);
    if (argc > 2) g_nthreads = atoi(argv[2]);
    if (argc > 3) g_initial_keys = atoi(argv[3]);

    expli::TM<int>::init();

    // Pre-populate (serial, no TM)
    for (int i = 0; i < g_initial_keys; i++) {
        int64_t key = (int64_t)(i * 2);
        BSTNode *z = alloc_node(key);
        if (!z) { fprintf(stderr, "OOM\n"); exit(1); }
        insert_serial(&g_root, z);
    }
    int init_count = tree_count(g_root);
    printf("BST Benchmark — Explicit TM API\n");
    printf("Duration: %d ms  Threads: %d  Initial keys: %d (actual: %d)\n",
           g_duration_ms, g_nthreads, g_initial_keys, init_count);

    // Run threads
    std::vector<std::thread> threads;
    std::vector<ThreadData> tdata(g_nthreads);
    for (int i = 0; i < g_nthreads; i++) {
        tdata[i].tid = i;
        threads.emplace_back(worker, std::ref(tdata[i]));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(g_duration_ms));
    g_stop.store(true);
    for (auto &th : threads) th.join();

    // Validate
    int final_count = tree_count(g_root);
    bool valid = tree_validate(g_root, INT64_MIN, INT64_MAX);

    int total_ops = 0;
    for (auto &d : tdata) total_ops += d.ops;

    printf("Tree size: %d  Total ops: %d  Ops/sec: %d\n",
           final_count, total_ops, (int)(total_ops * 1000 / (double)g_duration_ms));
    printf("%s: BST benchmark completed\n", valid ? "PASS" : "FAIL");

    expli::TM<int>::exit();
    return 0;
}
