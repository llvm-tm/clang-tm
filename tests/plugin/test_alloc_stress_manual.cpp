// Manually-instrumented version of test_alloc_stress.
// Compiled WITHOUT the LLVM plugin to verify the TM runtime works correctly
// when instrumentation is done manually at the C++ level.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <random>
#include <thread>
#include <vector>

// TinySTM function declarations (not globals — those come from TinySTM_runtime.cpp at link)
#include "../../backends/TinySTM/tinystm_wbctl.hpp"

// ---- Manual TM wrappers (bypassing plugin) ----
using namespace tinystm;
extern "C" void* tm_malloc(size_t size);
extern "C" void  tm_free(void* ptr);

static void tx_begin() {
    tinystm::jmpbuf = (sigjmp_buf *)malloc(sizeof(sigjmp_buf));
    sigsetjmp(*tinystm::jmpbuf, 0);
    tinystm::begin();
}

static bool tx_end() {
    return tinystm::commit();
}

static void tx_abort() {
    tinystm::abort_tx("manual");
    // never reached
}

// ---- Shared globals (same as test_alloc_stress) ----

std::vector<int64_t> g_vec;
std::atomic<int64_t> g_vec_total{0};
std::atomic<int64_t> g_vec_pushes{0};

std::vector<std::pair<int64_t, int64_t>> g_map_data;
std::atomic<int64_t> g_map_ops{0};

int64_t *g_raw_ptr = nullptr;
std::atomic<int64_t> g_raw_written{0};

// Sync
std::atomic<bool> g_start{false};
std::atomic<bool> g_stop{false};

const int64_t ITEMS_PER_VEC_TX = 200;
const int64_t MAP_INSERTS_PER_TX = 32;
const int64_t MAP_ERASES_PER_TX = 16;
const int64_t RAW_NEWS_PER_TX = 8;

// ---- Manual TM TX functions ----

// For a manual TM, we use sigsetjmp-based retry.
// Each TX function manually calls tm_read/tm_write for each load/store.
// This is tedious but proves whether the runtime works correctly.

// Helper: find in map (manual lower_bound with TM reads)
static int64_t map_find_pos(int64_t k) {
    int64_t lo = 0, hi = (int64_t)g_map_data.size();
    while (lo < hi) {
        int64_t mid = (lo + hi) / 2;
        int64_t key = tm_read_i8((uint64_t*)&g_map_data[(size_t)mid].first);
        if (key < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void manual_vec_push(int64_t val) {
    // Manually insert into vector: realloc if needed, then store
    size_t sz = g_vec.size();
    size_t cap = g_vec.capacity();
    if (sz >= cap) {
        size_t new_cap = cap == 0 ? 4 : cap * 2;
        int64_t *new_buf = (int64_t*)tm_malloc(new_cap * sizeof(int64_t));
        // Copy old elements via TM reads/writes
        for (size_t i = 0; i < sz; i++) {
            int64_t old = tm_read_i8((uint64_t*)&g_vec[i]);
            tm_write_i8((uint64_t*)&new_buf[i], old);
        }
        // Free old buffer (deferred if in TX, immediate if not)
        if (cap > 0)
            tm_free(g_vec.data());
        // Update vector internal pointers (raw stores, no TM needed for these)
        // But the plugin would instrument these too...
        // For manual version, we just do raw pointer swaps
        // This is tricky - we need to modify the vector's internal __begin_,
        // __end_, __end_cap_ pointers.  The plugin would instrument these.
        // For safety, use placement new to swap:
        struct VecRep {
            int64_t *begin_;
            int64_t *end_;
            int64_t *end_cap_;
        };
        VecRep *rep = (VecRep*)&g_vec;
        rep->begin_ = new_buf;
        rep->end_ = new_buf + sz;
        rep->end_cap_ = new_buf + new_cap;
    }
    // Store new element and bump end
    size_t pos = g_vec.size();
    tm_write_i8((uint64_t*)&g_vec[pos], val);
    struct VecRep {
        int64_t *begin_;
        int64_t *end_;
        int64_t *end_cap_;
    };
    VecRep *rep = (VecRep*)&g_vec;
    rep->end_ = rep->begin_ + pos + 1;
}

static void manual_map_insert(int64_t key, int64_t val) {
    // Manual binary search + insert
    int64_t pos = map_find_pos(key);
    if (pos < (int64_t)g_map_data.size() && g_map_data[(size_t)pos].first == key) {
        // Update existing
        tm_write_i8((uint64_t*)&g_map_data[(size_t)pos].second, val);
        return;
    }
    // Insert new entry at pos - realloc manually
    size_t sz = g_map_data.size();
    size_t cap = g_map_data.capacity();
    if (sz >= cap) {
        size_t new_cap = cap == 0 ? 4 : cap * 2;
        auto *new_buf = (std::pair<int64_t,int64_t>*)tm_malloc(new_cap * sizeof(std::pair<int64_t,int64_t>));
        for (size_t i = 0; i < sz; i++) {
            int64_t k = tm_read_i8((uint64_t*)&g_map_data[i].first);
            int64_t v = tm_read_i8((uint64_t*)&g_map_data[i].second);
            tm_write_i8((uint64_t*)&new_buf[i].first, k);
            tm_write_i8((uint64_t*)&new_buf[i].second, v);
        }
        if (cap > 0)
            tm_free(g_map_data.data());
        struct VecRep {
            std::pair<int64_t,int64_t> *begin_;
            std::pair<int64_t,int64_t> *end_;
            std::pair<int64_t,int64_t> *end_cap_;
        };
        VecRep *rep = (VecRep*)&g_map_data;
        rep->begin_ = new_buf;
        rep->end_ = new_buf + sz;
        rep->end_cap_ = new_buf + new_cap;
    }
    // Shift elements right
    for (int64_t i = (int64_t)sz; i > pos; i--) {
        int64_t k = tm_read_i8((uint64_t*)&g_map_data[(size_t)(i-1)].first);
        int64_t v = tm_read_i8((uint64_t*)&g_map_data[(size_t)(i-1)].second);
        tm_write_i8((uint64_t*)&g_map_data[(size_t)i].first, k);
        tm_write_i8((uint64_t*)&g_map_data[(size_t)i].second, v);
    }
    // Write new entry
    tm_write_i8((uint64_t*)&g_map_data[(size_t)pos].first, key);
    tm_write_i8((uint64_t*)&g_map_data[(size_t)pos].second, val);
    // Bump end
    struct VecRep {
        std::pair<int64_t,int64_t> *begin_;
        std::pair<int64_t,int64_t> *end_;
        std::pair<int64_t,int64_t> *end_cap_;
    };
    VecRep *rep = (VecRep*)&g_map_data;
    rep->end_ = rep->begin_ + sz + 1;
}

static void manual_map_erase(int64_t key) {
    int64_t pos = map_find_pos(key);
    if (pos >= (int64_t)g_map_data.size() || g_map_data[(size_t)pos].first != key)
        return;
    size_t sz = g_map_data.size();
    // Shift left
    for (size_t i = (size_t)pos; i + 1 < sz; i++) {
        int64_t k = tm_read_i8((uint64_t*)&g_map_data[i + 1].first);
        int64_t v = tm_read_i8((uint64_t*)&g_map_data[i + 1].second);
        tm_write_i8((uint64_t*)&g_map_data[i].first, k);
        tm_write_i8((uint64_t*)&g_map_data[i].second, v);
    }
    // Decrement end
    struct VecRep {
        std::pair<int64_t,int64_t> *begin_;
        std::pair<int64_t,int64_t> *end_;
        std::pair<int64_t,int64_t> *end_cap_;
    };
    VecRep *rep = (VecRep*)&g_map_data;
    rep->end_ = rep->begin_ + sz - 1;
}

#define MANUAL_TX(body) do { \
    tx_begin();              \
    body                     \
    if (!tx_end()) {         \
        tx_abort();          \
    }                        \
} while(0)

void manual_vec_push_tx(int64_t base) {
    MANUAL_TX({
        for (int64_t i = 0; i < ITEMS_PER_VEC_TX; i++)
            manual_vec_push(base + i);
    });
}

void manual_map_insert_tx(int64_t base) {
    MANUAL_TX({
        for (int64_t i = 0; i < MAP_INSERTS_PER_TX; i++)
            manual_map_insert(base + i, (base + i) * 10);
    });
}

void manual_map_erase_tx(int64_t base) {
    MANUAL_TX({
        for (int64_t i = 0; i < MAP_ERASES_PER_TX; i++)
            manual_map_erase(base + i);
    });
}

void manual_raw_new_delete_tx(int64_t val) {
    MANUAL_TX({
        for (int64_t i = 0; i < RAW_NEWS_PER_TX; i++) {
            int64_t *p = (int64_t*)tm_malloc(sizeof(int64_t));
            int64_t v = val + i;
            tm_write_i8((uint64_t*)p, v);
            v = tm_read_i8((uint64_t*)p);
            v++;
            tm_write_i8((uint64_t*)p, v);
            tm_free(p);
        }
    });
}

// ---- Workers ----

void vec_worker(int id) {
    std::mt19937 rng((unsigned)(id * 12345 + 1));
    while (!g_start.load()) std::this_thread::yield();
    while (!g_stop.load()) {
        int64_t base = (int64_t)rng() % 1000000;
        manual_vec_push_tx(base);
    }
}

void map_insert_worker(int id) {
    std::mt19937 rng((unsigned)(id * 12345 + 2));
    while (!g_start.load()) std::this_thread::yield();
    while (!g_stop.load()) {
        int64_t base = (int64_t)rng() % 1000000;
        manual_map_insert_tx(base);
    }
}

void map_erase_worker(int id) {
    std::mt19937 rng((unsigned)(id * 12345 + 3));
    while (!g_start.load()) std::this_thread::yield();
    while (!g_stop.load()) {
        int64_t base = (int64_t)rng() % 1000000;
        manual_map_erase_tx(base);
    }
}

void raw_new_delete_worker(int id) {
    std::mt19937 rng((unsigned)(id * 12345 + 4));
    while (!g_start.load()) std::this_thread::yield();
    while (!g_stop.load()) {
        int64_t val = (int64_t)rng() % 1000000;
        manual_raw_new_delete_tx(val);
    }
}

// ---- Main ----

int main(int argc, char *argv[]) {
    tinystm::init();

    int duration = 3;
    int n_vec = 1;
    int n_map_ins = 1;
    int n_raw = 1;

    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc) {
            if (strcmp(argv[i], "-d") == 0) duration = atoi(argv[++i]);
            else if (strcmp(argv[i], "-v") == 0) n_vec = atoi(argv[++i]);
            else if (strcmp(argv[i], "-i") == 0) n_map_ins = atoi(argv[++i]);
            else if (strcmp(argv[i], "-r") == 0) n_raw = atoi(argv[++i]);
        }
    }

    printf("Manual Alloc Stress Test\n");
    printf("Duration: %ds\n", duration);
    printf("  vec workers:       %d\n", n_vec);
    printf("  map insert workers: %d\n", n_map_ins);
    printf("  raw new/delete:     %d\n\n", n_raw);

    int total_threads = n_vec + n_map_ins + n_raw;
    std::vector<std::thread> threads;

    for (int i = 0; i < n_vec; i++)
        threads.emplace_back(vec_worker, i + 100);
    for (int i = 0; i < n_map_ins; i++)
        threads.emplace_back(map_insert_worker, i + 200);
    for (int i = 0; i < n_raw; i++)
        threads.emplace_back(raw_new_delete_worker, i + 300);

    g_start.store(true);
    std::this_thread::sleep_for(std::chrono::seconds(duration));
    g_stop.store(true);

    for (auto &t : threads)
        t.join();

    printf("\nResults:\n");
    printf("  g_vec.size() = %zu\n", g_vec.size());
    printf("  g_map_data.size() = %zu\n", g_map_data.size());

    bool ok = true;
    // Verify map keys
    for (size_t i = 1; i < g_map_data.size(); i++) {
        if (g_map_data[i].first <= g_map_data[i-1].first) {
            printf("  FAIL: map not sorted at %zu: %lld <= %lld\n",
                   i, (long long)g_map_data[i].first, (long long)g_map_data[i-1].first);
            ok = false;
        }
        if (g_map_data[i].second != g_map_data[i].first * 10) {
            printf("  FAIL: g_map[%lld] = %lld (expected %lld)\n",
                   (long long)g_map_data[i].first,
                   (long long)g_map_data[i].second,
                   (long long)(g_map_data[i].first * 10));
            ok = false;
        }
    }

    if (ok) {
        printf("\n  Result: PASS\n");
        return 0;
    }
    printf("\n  Result: FAIL\n");
    return 1;
}
