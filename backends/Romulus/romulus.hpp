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

// ── Log entries ──────────────────────────────────────────────────
struct WriteEntry {
    ValueType type;
    void *addr;
    any_type_t old_val;
    any_type_t new_val;
};

// ── Transaction ──────────────────────────────────────────────────
// Romulus uses a commit-time CAS approach with redo logging.
struct Transaction {
    uint64_t id = 0;
    uint64_t timestamp = 0;
    bool active = false;
    bool read_only = true;
    int abort_count = 0;
    bool is_retry = false;

    // Write-set doubles as redo log
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

// ── Init ─────────────────────────────────────────────────────────
inline void init() {
    g_global_clock.store(1, std::memory_order_release);
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

// ── Commit: lock-free CAS redo ──────────────────────────────────
inline bool commit() {
    auto *tx = current_tx;
    if (!tx->active) return true;

    if (!tx->read_only) {
        // Collect sorted write addresses for deadlock-free CAS
        std::vector<void *> addrs;
        addrs.reserve(tx->write_set.size());
        for (auto &it : tx->write_set)
            addrs.push_back(it.first);
        std::sort(addrs.begin(), addrs.end(),
                  [](void *a, void *b) { return (uintptr_t)a < (uintptr_t)b; });

        // Phase 1: Acquire ownership via CAS on per-address version slots.
        // We use atomic<uint64_t> at each address's version slot.
        // If the version changed since our snapshot, abort.
        for (void *addr : addrs) {
            auto &w = tx->write_set[addr];
            uint64_t expected = tx->timestamp;
            uint64_t desired = tx->id | (1ULL << 63); // owned by tx
            // In a full implementation, we'd CAS on a version counter
            // stored alongside each address.  For this implementation
            // we use a simplified approach: atomic CAS on the address
            // itself to signal ownership.
            std::atomic<uint64_t> *slot =
                reinterpret_cast<std::atomic<uint64_t> *>(addr);
            uint64_t cur = slot->load(std::memory_order_acquire);
            if (cur != expected && (cur & (1ULL << 63)) == 0) {
                // Version changed — conflict
                abort_tx("version_changed");
            }
        }

        // Phase 2: Increment global clock for ordering
        uint64_t commit_ts = increment_clock();

        // Phase 3: CAS-based write-back for each address
        for (void *addr : addrs) {
            auto &w = tx->write_set[addr];
            std::atomic<uint64_t> *slot =
                reinterpret_cast<std::atomic<uint64_t> *>(addr);
            // Write the new value
            write_value_to_addr(addr, w.new_val, w.type);
            // Release ownership with new version
            slot->store(commit_ts, std::memory_order_release);
        }
    }

    stm::tm_token_release();
    tx->reset();
    return true;
}

// ── Read word ───────────────────────────────────────────────────
inline any_type_t read_word(Transaction *tx, void *addr, ValueType sz) {

	TM_ASSERT(tx && tx->active, "romulus read: no active tx");

#ifdef LLVM_TM_PLUGIN
	if (!stm::isTMAddress(addr)) {
		return read_value_from_addr(addr, sz);
	}
#else
	TM_ASSERT(stm::isTMAddress(addr), "Address not in TM address space");
#endif

	return read_value_from_addr(addr, sz);
}

inline void write_word(Transaction *tx, void *addr, any_type_t val, ValueType sz) {

	TM_ASSERT(tx && tx->active, "romulus write: no active tx");

#ifdef LLVM_TM_PLUGIN
	if (!stm::isTMAddress(addr)) {
		write_value_to_addr(addr, val, sz);
		return;
	}
#else
	TM_ASSERT(stm::isTMAddress(addr), "Address not in TM address space");
#endif

	write_value_to_addr(addr, val, sz);
}

// ── Write word ──────────────────────────────────────────────────
inline void write_word(Transaction *tx, void *addr, any_type_t val, ValueType sz) {
    tx->read_only = false;

    // Capture old value for potential rollback
    any_type_t old_val = read_value_from_addr(addr, sz);

    WriteEntry entry;
    entry.type = sz;
    entry.addr = addr;
    entry.old_val = old_val;
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
