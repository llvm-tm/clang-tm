#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../tm_common.hpp"
#include "../tm_spin_token.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
}

namespace xtm {

using stm::ValueType;
using stm::any_type_t;
using stm::fill_any_type;
using stm::read_value_from_addr;
using stm::return_any_type;
using stm::type_size;
using stm::write_value_to_addr;

// ── jmpbuf (defined in runtime) ─────────────────────────────────
extern __thread sigjmp_buf *jmpbuf_ptr;

// ── Version tracking ────────────────────────────────────────────
// Each address has a version that increments on every write.
// XTM uses these versions to build a dependency DAG.

// ── Log entries ──────────────────────────────────────────────────
struct ReadLogEntry {
    ValueType type;
    void *addr;
    uint64_t observed_version;
};

struct WriteLogEntry {
    ValueType type;
    void *addr;
    any_type_t new_val;
    uint64_t version;
};

// ── Version slot (per-address version counter) ──────────────────
// In a full implementation these would be stored alongside data.
// Here we use a global array indexed by address hash.
constexpr size_t VERSION_TABLE_SIZE = 1ULL << 20;
extern std::atomic<uint64_t> g_version_table[VERSION_TABLE_SIZE];

inline size_t version_index(void *addr) {
    return ((uintptr_t)addr >> 3) & (VERSION_TABLE_SIZE - 1);
}

// ── Transaction ──────────────────────────────────────────────────
struct Transaction {
    uint64_t id = 0;
    uint64_t left_id = 0;    // Left phase identifier
    uint64_t right_id = 0;   // Right phase identifier
    uint64_t start_version = 0;
    uint64_t end_version = 0;
    bool active = false;
    bool read_only = true;
    int abort_count = 0;
    bool is_retry = false;

    std::unordered_map<void *, ReadLogEntry> read_set;
    std::unordered_map<void *, WriteLogEntry> write_set;

    // Dependency tracking: addresses read whose versions changed
    std::vector<void *> deps;

    void reset() {
        start_version = 0;
        end_version = 0;
        left_id = 0;
        right_id = 0;
        active = false;
        read_only = true;
        abort_count = 0;
        is_retry = false;
        deps.clear();
        clear();
    }

    void clear() {
        read_set.clear();
        write_set.clear();
        deps.clear();
    }
};

// ── Globals ──────────────────────────────────────────────────────
extern std::atomic<uint64_t> g_clock;
extern std::atomic<uint64_t> thr_counter;
extern std::atomic<uint64_t> g_phase;
extern __thread Transaction *current_tx;
extern std::atomic<uint64_t> g_tm_abort_count;

// ── Init ─────────────────────────────────────────────────────────
inline void init() {
    g_clock.store(1, std::memory_order_release);
    thr_counter.store(1, std::memory_order_release);
    g_phase.store(0, std::memory_order_release);
    for (size_t i = 0; i < VERSION_TABLE_SIZE; i++)
        g_version_table[i].store(0, std::memory_order_release);
}

inline void exit() {}

inline void init_thread() {
    if (!current_tx) {
        current_tx = new Transaction();
        current_tx->id = thr_counter.fetch_add(1, std::memory_order_acq_rel);
    }
    current_tx->reset();
}

inline void exit_thread() {
    delete current_tx;
    current_tx = nullptr;
}

// ── Clock helpers ────────────────────────────────────────────────
inline uint64_t get_clock() {
    return g_clock.load(std::memory_order_acquire);
}

inline uint64_t increment_clock() {
    return g_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// ── Left-Right phases ───────────────────────────────────────────
inline uint64_t enter_left_phase() {
    uint64_t phase = g_phase.fetch_add(1, std::memory_order_acq_rel);
    return phase;
}

inline uint64_t enter_right_phase() {
    // Spin until all threads have entered left phase
    while (g_phase.load(std::memory_order_acquire) <
           thr_counter.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return g_phase.fetch_add(1, std::memory_order_acq_rel);
}

// ── Begin ────────────────────────────────────────────────────────
inline bool begin() {
    auto *tx = current_tx;
    if (tx->active) return true;

    tx->clear();
    tx->start_version = get_clock();
    tx->end_version = tx->start_version;
    tx->left_id = enter_left_phase();
    tx->active = true;
    tx->read_only = true;
    if (!tx->is_retry) tx->abort_count = 0;
    tx->is_retry = false;
    return true;
}

// ── Abort ────────────────────────────────────────────────────────
inline void abort_tx(const char *loc = "") {
    auto *tx = current_tx;
    tx->clear();
    tx->abort_count++;
    tx->is_retry = true;
    tx->active = false;
    stm::tm_token_release_if_held(tx->id);
    g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
    siglongjmp(*jmpbuf_ptr, 1);
}

// ── Validate ────────────────────────────────────────────────────
inline bool validate() {
    auto *tx = current_tx;
    for (auto &it : tx->read_set) {
        auto &r = it.second;
        size_t idx = version_index(r.addr);
        uint64_t ver = g_version_table[idx].load(std::memory_order_acquire);
        if (ver != r.observed_version)
            return false;
    }
    return true;
}

// ── Extend ──────────────────────────────────────────────────────
inline bool extend() {
    auto *tx = current_tx;
    uint64_t last_version = get_clock();
    if (!validate()) return false;
    tx->end_version = last_version;
    return true;
}

// ── Commit ──────────────────────────────────────────────────────
// XTM uses a 3-phase commit:
//   1. Left phase: reads and version tracking captured during begin()
//   2. Right phase (enter): ensure all threads crossed the barrier
//   3. Commit: validate deps, write-back, release
inline bool commit() {
    auto *tx = current_tx;
    if (!tx->active) return true;

    if (!tx->read_only) {
        // Phase 1: Cross to right phase (barrier)
        tx->right_id = enter_right_phase();

        // Phase 2: Validate read-set versions
        for (auto &it : tx->read_set) {
            auto &r = it.second;
            size_t idx = version_index(r.addr);
            uint64_t ver = g_version_table[idx].load(std::memory_order_acquire);
            if (ver != r.observed_version) {
                // Dependency changed — abort
                abort_tx("dep_changed");
            }
        }

        // Phase 3: Increment clock for ordering
        uint64_t commit_version = increment_clock();

        // Phase 4: Write-back with version updates
        for (auto &it : tx->write_set) {
            auto &addr = it.first;
            auto &w = it.second;
            write_value_to_addr(addr, w.new_val, w.type);
            size_t idx = version_index(addr);
            g_version_table[idx].store(commit_version, std::memory_order_release);
        }
    }

    stm::tm_token_release();
    tx->reset();
    return true;
}

// ── Read word ───────────────────────────────────────────────────
inline any_type_t read_word(Transaction *tx, void *addr, ValueType sz) {
    // Check write-set
    auto w = tx->write_set.find(addr);
    if (w != tx->write_set.end())
        return w->second.new_val;

    // Read from memory
    any_type_t val = read_value_from_addr(addr, sz);

    // Track version for dependency detection
    size_t idx = version_index(addr);
    uint64_t ver = g_version_table[idx].load(std::memory_order_acquire);

    // Add to read-set
    ReadLogEntry entry;
    entry.type = sz;
    entry.addr = addr;
    entry.observed_version = ver;
    tx->read_set[addr] = entry;

    return val;
}

// ── Write word ──────────────────────────────────────────────────
inline void write_word(Transaction *tx, void *addr, any_type_t val, ValueType sz) {
    tx->read_only = false;
    size_t idx = version_index(addr);
    uint64_t ver = g_version_table[idx].load(std::memory_order_acquire);

    WriteLogEntry entry;
    entry.type = sz;
    entry.addr = addr;
    entry.new_val = val;
    entry.version = ver;
    tx->write_set[addr] = entry;
}

// ── Typed read/write templates ──────────────────────────────────
template <typename T, ValueType SZ>
inline T tm_read(T *addr) {
    any_type_t v = read_word(current_tx, (void *)addr, SZ);
    return return_any_type<T>(v);
}

template <typename T, ValueType SZ>
inline void tm_write(T *addr, T val) {
    any_type_t w;
    fill_any_type(w, &val, SZ);
    write_word(current_tx, (void *)addr, w, SZ);
}

} // namespace xtm
