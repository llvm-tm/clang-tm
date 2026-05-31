// Fuzz alloc — concurrent sorted linked-list insert/erase inside TXs.
// Exercises tm_malloc / tm_free (spec_alloc + deferred-free) under
// contention, triggering double-free / use-after-free patterns.
//
// Build via fuzz_runner.py, or manually:
//   clang++ -std=c++20 -O0 -pthread -g -I$(PWD) \
//       -DTM_EVENT_LOG -DTM_BACKEND_TL2 -Ibackends/TL2 \
//       tools/stm_bug_tool/benchmarks/fuzz_alloc.cpp \
//       backends/runtimes/tl2_runtime.cpp

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

#include "backends/tm_event_logger.hpp"

// ── Runtime extern declarations (matching runtime definitions, no symbol_id) ──
extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;

void tm_init();
void tm_exit();
void tm_init_thread();
void tm_exit_thread();
void tm_begin();
void tm_end();

// Read/write primitives (no symbol_id for direct-compiled tests)
uint8_t   tm_read_i1(uint8_t *addr);
uint16_t  tm_read_i2(uint16_t *addr);
uint32_t  tm_read_i4(uint32_t *addr);
uint64_t  tm_read_i8(uint64_t *addr);
float     tm_read_f4(float *addr);
double    tm_read_f8(double *addr);
void     *tm_read_ptr(void **addr);

void tm_write_i1(uint8_t *addr, uint8_t val);
void tm_write_i2(uint16_t *addr, uint16_t val);
void tm_write_i4(uint32_t *addr, uint32_t val);
void tm_write_i8(uint64_t *addr, uint64_t val);
void tm_write_f4(float *addr, float val);
void tm_write_f8(double *addr, double val);
void tm_write_ptr(void **addr, void *val);

void* tm_malloc(size_t size);
void* tm_calloc(size_t nmemb, size_t size);
void* tm_realloc(void* ptr, size_t size);
void  tm_free(void* ptr);
}

// ── Retry loop (simplified from test_helpers.hpp) ────────────────
template <typename F>
inline void tm_transaction(F&& body) {
    int committed = 0;
    while (!committed) {
        tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
        tm_begin();
        if (tm_longjmp_ret != 0)
            continue;
        body();
        tm_end();
        committed = 1;
    }
}

// Convenience wrappers
inline uint32_t  tm_r4(uint32_t *a)  { return tm_read_i4(a); }
inline uint64_t  tm_r8(uint64_t *a)  { return tm_read_i8(a); }
inline void      tm_w4(uint32_t *a, uint32_t v)  { tm_write_i4(a, v); }
inline void      tm_w8(uint64_t *a, uint64_t v)  { tm_write_i8(a, v); }
inline void     *tm_rp(void **a)     { return tm_read_ptr(a); }
inline void      tm_wp(void **a, void *v) { tm_write_ptr(a, v); }

// ── Sorted linked-list node ──────────────────────────────────────
struct Node {
    int key;
    Node* next;
};

static std::atomic<Node*> g_head{nullptr};
static int g_key_range;

static std::atomic<uint64_t> g_total_inserts{0};
static std::atomic<uint64_t> g_total_erases{0};

// ── Find predecessor for key in sorted list ─────────────────────
struct FindResult { Node* prev; Node* curr; };

static FindResult find_key(int key, Node* head) {
    Node* prev = nullptr;
    Node* curr = head;
    while (curr && curr->key < key) {
        prev = curr;
        curr = curr->next;
    }
    return {prev, curr};
}

// ── Sorted insert ──────────────────────────────────────────────
static bool tx_insert(int key, Node* head) {
    auto [prev, curr] = find_key(key, head);
    if (curr && curr->key == key) return false;

    Node* node = (Node*)tm_malloc(sizeof(Node));
    node->key = key;
    node->next = curr;

    if (prev)
        tm_write_ptr(reinterpret_cast<void**>(&prev->next), node);
    else
        tm_write_ptr(reinterpret_cast<void**>(&g_head), node);
    return true;
}

// ── Sorted erase ───────────────────────────────────────────────
static bool tx_erase(int key, Node* head) {
    auto [prev, curr] = find_key(key, head);
    if (!curr || curr->key != key) return false;

    Node* next = curr->next;
    if (prev)
        tm_write_ptr(reinterpret_cast<void**>(&prev->next), next);
    else
        tm_write_ptr(reinterpret_cast<void**>(&g_head), next);

    tm_free(curr);
    return true;
}

// ── Walk list and verify invariants ────────────────────────────
static int verify_list() {
    Node* head = g_head.load(std::memory_order_acquire);
    int max_nodes = g_key_range * 2 + 4;
    std::set<int> seen;
    Node* prev = nullptr;
    Node* curr = head;
    int count = 0;

    while (curr && count < max_nodes) {
        int k = curr->key;
        if (k < 0 || k >= g_key_range * 2 + 4) {
            printf("CORRUPT: key=%d out of range at node=%p\n", k, (void*)curr);
            return -1;
        }
        if (prev && curr->key <= prev->key) {
            printf("CORRUPT: unsorted at key=%d (prev=%d)\n", curr->key, prev->key);
            return -1;
        }
        if (seen.count(curr->key)) {
            printf("CORRUPT: duplicate key=%d\n", curr->key);
            return -1;
        }
        seen.insert(curr->key);
        prev = curr;
        curr = curr->next;
        count++;
    }
    if (count >= max_nodes) {
        printf("CORRUPT: cycle detected (>=%d nodes)\n", max_nodes);
        return -1;
    }
    return count;
}

int main(int argc, char **argv) {
    int num_threads = argc > 1 ? atoi(argv[1]) : 4;
    int iters = argc > 2 ? atoi(argv[2]) : 1000;
    g_key_range = argc > 3 ? atoi(argv[3]) : 64;
    unsigned seed = argc > 4 ? atoi(argv[4]) : 42;

    tm_init();
    TM_EVENT_INSTALL_SIGSEGV();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([t, iters, seed]() {
            tm_init_thread();
            tm_nested_call_counter++;
            unsigned local_seed = seed + t * 1000;
            uint64_t inserts = 0, erases = 0;

            for (int i = 0; i < iters; i++) {
                int key = rand_r(&local_seed) % g_key_range;
                int op = rand_r(&local_seed) % 2;

                tm_transaction([&]() {
                    Node* head = g_head.load(std::memory_order_acquire);
                    if (op == 0) {
                        if (tx_insert(key, head)) inserts++;
                    } else {
                        if (tx_erase(key, head)) erases++;
                    }
                });
            }

            g_total_inserts.fetch_add(inserts, std::memory_order_relaxed);
            g_total_erases.fetch_add(erases, std::memory_order_relaxed);
            TM_EVENT_DUMP(0);
            tm_nested_call_counter--;
            tm_exit_thread();
        });
    }
    for (auto &th : threads) th.join();

    // ── Final verification ─────────────────────────────────────
    int node_count = verify_list();
    if (node_count < 0) {
        printf("INVARIANT: list integrity: FAIL (corrupted list)\n");
        TM_EVENT_DUMP(0);
        tm_exit();
        return 1;
    }

    uint64_t ni = g_total_inserts.load();
    uint64_t ne = g_total_erases.load();

    printf("INVARIANT: list integrity: PASS (%d nodes, %llu ins, %llu del)\n",
           node_count, (unsigned long long)ni, (unsigned long long)ne);

    TM_EVENT_DUMP(0);
    tm_exit();
    return 0;
}
