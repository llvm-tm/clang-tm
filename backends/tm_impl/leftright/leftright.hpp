#pragma once

#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../common/tm_common.hpp"
#include "../common/tm_spin_token.hpp"

extern __thread int32_t tm_nested_call_counter;

// Global (non-TLS) queue-active flag (defined in queue_runtime.cpp).
// Visible to all threads — unlike the TLS g_tm_queue_active which is
// only set for the enqueuing thread.
extern std::atomic<int> g_tm_queue_global;

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
    any_type_t captured_value;
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

// Queue mode active (1 when running under tm-instrument-queue* pipeline).
// Only worker threads run TM transactions; the main thread never enters
// tm_begin/tm_end, so barriers that expect ALL threads would deadlock.
// Uses the global (non-TLS) flag so all worker threads see the same value.
inline bool isQueueActive() { return g_tm_queue_global.load(std::memory_order_acquire); }

// ── Global commit lock (serializes writers) ─────────────────────
extern std::atomic<uint32_t> g_commit_lock;

// ── Init ─────────────────────────────────────────────────────────
inline void init() {
    g_clock.store(1, std::memory_order_release);
    thr_counter.store(1, std::memory_order_release);
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

inline bool acquire_commit_lock() {
    uint32_t expected = 0;
    return g_commit_lock.compare_exchange_strong(expected, 1,
        std::memory_order_acquire,
        std::memory_order_relaxed);
}

inline void release_commit_lock() {
    g_commit_lock.store(0, std::memory_order_release);
}

inline bool commit() {
    auto *tx = current_tx;
    if (!tx->active) return true;

    if (!tx->read_only) {
        if (isQueueActive()) {
            // Queue mode: the executor provides ordering (each worker
            // runs one TX at a time).  Validate and write-back directly.
            if (!validate())
                abort_tx("read_validation");
            uint64_t commit_version = increment_clock();
            for (auto &it : tx->write_set) {
                auto &addr = it.first;
                auto &w = it.second;
                write_value_to_addr(addr, w.new_val, w.type);
            }
        } else {
            // Sort write-set addresses (deterministic ordering for
            // potential future per-address locking).
            std::vector<void *> sorted_addrs;
            sorted_addrs.reserve(tx->write_set.size());
            for (auto &it : tx->write_set)
                sorted_addrs.push_back(it.first);
            std::sort(sorted_addrs.begin(), sorted_addrs.end(), compareByAddr);

            // Phase 1: Optimistic validation (outside lock).
            // Cheap abort for already-stale transactions.
            if (!validate()) {
                stm::tm_token_release_if_held(tx->id);
                abort_tx("read_validation");
            }

            // Phase 2: Acquire global commit lock (serializes writers).
            while (!acquire_commit_lock())
                std::this_thread::yield();

            // Phase 3: Re-validate under lock.
            // Catches changes since Phase 1 (concurrent commit could have
            // modified a read-set address between the validate and here).
            bool valid = validate();

            // Value-based validation: re-read all read-set addresses and
            // compare with captured values.  This detects actual conflicts
            // even when the clock didn't advance between reads (the "commit
            // after all reads" case), without the false aborts caused by
            // a simple get_clock() > end_version check (which fires on
            // every concurrent commit, even non-conflicting ones).
            if (valid) {
                for (auto &it : tx->read_set) {
                    auto &r = it.second;
                    any_type_t cur = read_value_from_addr(r.addr, r.type);
                    if (std::memcmp(&cur, &r.captured_value,
                                    type_size(r.type)) != 0) {
                        valid = false;
                        break;
                    }
                }
            }

            if (valid) {
                // Phase 4: Increment clock and write-back.
                uint64_t commit_version = increment_clock();
                for (auto &it : tx->write_set) {
                    auto &addr = it.first;
                    auto &w = it.second;
                    write_value_to_addr(addr, w.new_val, w.type);
                }
            }

            // Phase 5: Release lock.  MUST happen before abort_tx()
            // (which does siglongjmp), otherwise the lock is held forever.
            release_commit_lock();

            if (!valid) {
                stm::tm_token_release_if_held(tx->id);
                abort_tx("read_validation");
            }
        }
    }

    stm::tm_token_release();
    tx->reset();
    return true;
}

// ── Read word ───────────────────────────────────────────────────
inline any_type_t read_word(Transaction *tx, void *addr, ValueType sz) {
    TM_ASSERT(tx && tx->active, "leftright read: no active tx");

    // Check write set first — own writes must be visible
    auto wit = tx->write_set.find(addr);
    if (wit != tx->write_set.end()) {
        return wit->second.new_val;
    }

    if (!stm::isTMAddress(addr))
        return read_value_from_addr(addr, sz);

    any_type_t val = read_value_from_addr(addr, sz);

    // ARM read-data reorder barrier: prevents the plain data load from
    // being reordered after the subsequent acquire load in get_clock().
    // Without this, a reader could see data from version N+1 but a
    // stale clock version N, causing OCC validation to pass incorrectly.
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (!isQueueActive()) {
        // Non-queue mode: log reads for OCC read-set validation.
        // In queue mode the read-set is not needed (validation is a no-op
        // since the queue provides ordering; the lock is skipped too).
        auto r = tx->read_set.find(addr);
        if (r == tx->read_set.end()) {
            ReadLogEntry entry;
            entry.type = sz;
            entry.addr = addr;
            entry.observed_version = get_clock();
            entry.captured_value = val;
            tx->read_set[addr] = entry;
        }
    }
    return val;
}

// ── Write word ──────────────────────────────────────────────────
inline void write_word(Transaction *tx, void *addr, any_type_t val, ValueType sz, bool skip_write_set = false) {
    TM_ASSERT(tx && tx->active, "leftright write: no active tx");

    tx->read_only = false;

    if (!stm::isTMAddress(addr)) {
        write_value_to_addr(addr, val, sz);
        return;
    }

    // In queue mode with auto-wait (no concurrent TXes), writes can be
    // applied directly instead of buffered.  But the backend cannot
    // distinguish auto from manual, so always buffer for safety.
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
