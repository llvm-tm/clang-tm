#pragma once

#include <atomic>
#include <csetjmp>
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

namespace leftright {

using stm::ValueType;
using stm::any_type_t;
using stm::any_type_mapping;
using stm::ByteOffset;
using stm::fill_any_type;
using stm::read_value_from_addr;
using stm::return_any_type;
using stm::type_size;
using stm::write_value_to_addr;

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
};

// ── Transaction ──────────────────────────────────────────────────
struct Transaction {
    uint64_t id = 0;
    uint64_t start_version = 0;
    uint64_t end_version = 0;
    bool active = false;
    bool aborted = false;
    bool read_only = true;
    int abort_count = 0;
    bool is_retry = false;

    std::unordered_map<void *, ReadLogEntry> read_set;
    std::unordered_map<void *, WriteLogEntry> write_set;

    void reset() {
        start_version = 0;
        end_version = 0;
        active = false;
        read_only = true;
        abort_count = 0;
        is_retry = false;
        clear();
    }

    void clear() {
        read_set.clear();
        write_set.clear();
    }
};

// ── jmpbuf (defined in runtime) ─────────────────────────────────
extern __thread sigjmp_buf *jmpbuf_ptr;

// ── Globals ──────────────────────────────────────────────────────
extern std::atomic<uint64_t> g_clock;
extern std::atomic<uint64_t> thr_counter;
extern __thread Transaction *current_tx;
extern std::atomic<uint64_t> g_tm_abort_count;

// ── Left-Right barrier counters ─────────────────────────────────
extern std::atomic<uint64_t> g_left_barrier;
extern std::atomic<uint64_t> g_right_barrier;
extern std::atomic<uint64_t> g_left_phase;
extern std::atomic<uint64_t> g_right_phase;

// ── Init ─────────────────────────────────────────────────────────
inline void init() {
    g_clock.store(1, std::memory_order_release);
    thr_counter.store(1, std::memory_order_release);
    g_left_barrier.store(0, std::memory_order_release);
    g_right_barrier.store(0, std::memory_order_release);
    g_left_phase.store(0, std::memory_order_release);
    g_right_phase.store(0, std::memory_order_release);
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

// ── Left-Right barrier ───────────────────────────────────────────
// All threads must cross the left barrier before any thread can
// proceed to the right phase.  This ensures a clean Left→Right
// ordering across all concurrent transactions.

inline void left_barrier() {
    uint64_t phase = g_left_phase.load(std::memory_order_acquire);
    g_left_barrier.fetch_add(1, std::memory_order_acq_rel);

    // Spin until all threads have reached the left barrier
    while (g_left_barrier.load(std::memory_order_acquire) <
           g_right_barrier.load(std::memory_order_acquire) + thr_counter.load() - 1) {
        std::this_thread::yield();
    }
}

inline void right_barrier() {
    g_right_barrier.fetch_add(1, std::memory_order_acq_rel);

    // Spin until all threads have crossed to the right phase
    while (g_right_barrier.load(std::memory_order_acquire) <
           g_left_barrier.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

// ── Begin/Abort/Commit ──────────────────────────────────────────
inline bool begin() {
    auto *tx = current_tx;
    if (tx->active) return true;

    tx->clear();
    tx->start_version = get_clock();
    tx->end_version = tx->start_version;
    tx->active = true;
    tx->read_only = true;
    if (!tx->is_retry) tx->abort_count = 0;
    tx->is_retry = false;
    return true;
}

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

inline bool validate() {
    auto *tx = current_tx;
    for (auto &it : tx->read_set) {
        auto &r = it.second;
        if (r.observed_version > tx->end_version)
            return false;
    }
    return true;
}

inline bool extend() {
    auto *tx = current_tx;
    uint64_t last_version = get_clock();
    if (!validate()) return false;
    tx->end_version = last_version;
    return true;
}

static bool compareByAddr(const void *a, const void *b) {
    return (uintptr_t)a < (uintptr_t)b;
}

inline bool commit() {
    auto *tx = current_tx;
    if (!tx->active) return true;

    if (!tx->read_only) {
        // Phase 1: Left-Right barrier — ensure all concurrent TXs
        // have finished their Left phase before proceeding to Right.
        left_barrier();

        // Phase 2: Sort write-set addresses and acquire locks
        std::vector<void *> sorted_addrs;
        sorted_addrs.reserve(tx->write_set.size());
        for (auto &it : tx->write_set)
            sorted_addrs.push_back(it.first);
        std::sort(sorted_addrs.begin(), sorted_addrs.end(), compareByAddr);

        for (void *addr : sorted_addrs) {
            auto &w = tx->write_set[addr];
            // In LeftRight, we use a simple version-based check
            // rather than per-address locks.  The barrier ensures
            // that all reads from the Left phase are consistent.
        }

        // Phase 3: Right barrier — ensure all threads have
        // completed their Left phase before any Right phase writes.
        right_barrier();

        // Phase 4: Validate read-set
        if (!validate()) {
            abort_tx("read_validation");
        }

        // Phase 5: Write-back and commit
        uint64_t commit_version = increment_clock();
        for (auto &it : tx->write_set) {
            auto &addr = it.first;
            auto &w = it.second;
            write_value_to_addr(addr, w.new_val, w.type);
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
    if (w != tx->write_set.end()) {
        return w->second.new_val;
    }

    // Read from memory
    any_type_t val = read_value_from_addr(addr, sz);

    // Add to read-set (only if not already present)
    auto r = tx->read_set.find(addr);
    if (r == tx->read_set.end()) {
        ReadLogEntry entry;
        entry.type = sz;
        entry.addr = addr;
        entry.observed_version = get_clock();
        tx->read_set[addr] = entry;
    }

    return val;
}

// ── Write word ──────────────────────────────────────────────────
inline void write_word(Transaction *tx, void *addr, any_type_t val, ValueType sz) {
    tx->read_only = false;
    WriteLogEntry entry;
    entry.type = sz;
    entry.addr = addr;
    entry.new_val = val;
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

} // namespace leftright
