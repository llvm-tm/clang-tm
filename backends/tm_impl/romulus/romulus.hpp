#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "tm_common.hpp"
#include "tm_spin_token.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
}

namespace romulus {

using stm::ValueType;
using stm::any_type_t;
using stm::any_type_mapping;
using stm::fill_any_type;
using stm::read_value_from_addr;
using stm::return_any_type;
using stm::type_size;
using stm::write_value_to_addr;

// ── jmpbuf (defined in runtime) ─────────────────────────────────
extern __thread sigjmp_buf *jmpbuf_ptr;

// ── Version table (separate from data, maps address→version) ───
constexpr size_t VERSION_TABLE_SIZE = 1 << 20;

inline size_t version_index(void *addr) {
    return (reinterpret_cast<uintptr_t>(addr) >> 3) & (VERSION_TABLE_SIZE - 1);
}

// ── Log entry ───────────────────────────────────────────────────
struct WriteEntry {
    ValueType type;
    void *addr;
    any_type_t new_val;
};

// ── Transaction ─────────────────────────────────────────────────
struct Transaction {
    uint64_t id = 0;
    uint64_t timestamp = 0;
    bool active = false;
    bool read_only = true;
    int abort_count = 0;
    bool is_retry = false;

    std::unordered_map<void *, WriteEntry> write_set;

    void reset() {
        timestamp = 0;
        active = false;
        read_only = true;
        abort_count = 0;
        is_retry = false;
        clear();
    }

    void clear() { write_set.clear(); }
};

// ── Globals ──────────────────────────────────────────────────────
extern std::atomic<uint64_t> g_global_clock;
extern std::atomic<uint64_t> thr_counter;
extern __thread Transaction *current_tx;
extern std::atomic<uint64_t> g_tm_abort_count;
extern std::atomic<uint64_t> *g_version_table;
extern std::atomic<uint64_t> g_commit_lock;

// ── Init ─────────────────────────────────────────────────────────
inline void init() {
    g_global_clock.store(1, std::memory_order_release);
    thr_counter.store(1, std::memory_order_release);
    if (!g_version_table) {
        g_version_table = new std::atomic<uint64_t>[VERSION_TABLE_SIZE];
        for (size_t i = 0; i < VERSION_TABLE_SIZE; i++)
            g_version_table[i].store(0, std::memory_order_release);
    }
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
    return g_global_clock.load(std::memory_order_acquire);
}

inline uint64_t increment_clock() {
    return g_global_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// ── Begin ────────────────────────────────────────────────────────
inline bool begin() {
    auto *tx = current_tx;
    if (tx->active) return true;

    tx->clear();
    tx->timestamp = get_clock();
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

// ── Commit: lock-based redo with version-table validation ──────
inline bool commit() {
    auto *tx = current_tx;
    if (!tx->active) return true;

    if (!tx->read_only && !tx->write_set.empty()) {
        // Collect sorted write addresses for deadlock-free version check
        std::vector<void *> addrs;
        addrs.reserve(tx->write_set.size());
        for (auto &it : tx->write_set)
            addrs.push_back(it.first);
        std::sort(addrs.begin(), addrs.end(),
                  [](void *a, void *b) { return (uintptr_t)a < (uintptr_t)b; });

        // Phase 1: Acquire commit lock
        while (g_commit_lock.load(std::memory_order_acquire) != 0 ||
               g_commit_lock.exchange(1, std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }

        // Phase 2: Validate
        bool valid = true;
        for (void *addr : addrs) {
            size_t idx = version_index(addr);
            uint64_t ver = g_version_table[idx].load(std::memory_order_acquire);
            if (ver > tx->timestamp) {
                valid = false;
                fprintf(stderr, "ROMULUS_VALFAIL: ts=%lu ver=%lu idx=%zu addr=%p\n",
                        (unsigned long)tx->timestamp, (unsigned long)ver, idx, addr);
                break;
            }
        }

        if (!valid) {
            g_commit_lock.store(0, std::memory_order_release);
            abort_tx("version_changed");
            return false;
        }

        // Phase 3: Increment global clock
        uint64_t commit_ts = increment_clock();

        // Phase 4: Write-back
        for (void *addr : addrs) {
            auto &w = tx->write_set[addr];
            write_value_to_addr(addr, w.new_val, w.type);
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Phase 5: Update version table
        for (void *addr : addrs) {
            size_t idx = version_index(addr);
            g_version_table[idx].store(commit_ts, std::memory_order_release);
        }

        // Release commit lock
        g_commit_lock.store(0, std::memory_order_release);
    }

    stm::tm_token_release();
    tx->reset();
    return true;
}

// ── Read word ───────────────────────────────────────────────────
inline any_type_t read_word(Transaction *tx, void *addr, ValueType sz) {

	TM_ASSERT(tx && tx->active, "romulus read: no active tx");

	// Check write set first — own writes must be visible
	auto wit = tx->write_set.find(addr);
	if (wit != tx->write_set.end()) {
		return wit->second.new_val;
	}

	if (!stm::isTMAddress(addr)) {
		return read_value_from_addr(addr, sz);
	}

	return read_value_from_addr(addr, sz);
}

// ── Write word (with write-set logging) ─────────────────────────
inline void write_word(Transaction *tx, void *addr, any_type_t val, ValueType sz) {
    tx->read_only = false;

    WriteEntry entry;
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

} // namespace romulus
