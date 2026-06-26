#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

#include "../common/tm_common.hpp"
#include "../common/tm_spin_token.hpp"

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

// ═══════════════════════════════════════════════════════════════
//  XTM — eXtended Transactional Memory (ASPLOS 2006)
//
//  Key data structures from the paper:
//    XSW — Transaction Status Word (per-thread state)
//    XADT — Transaction Address/Data Table (global page table)
//    XF  — XADT Filter (bloom filter for fast negative lookup)
//
//  Algorithm:
//    Page-granularity lazy versioning with private page copies.
//    On first write to a page, we take ownership via CAS and
//    create a private copy.  Reads take a version snapshot for
//    later validation.  Commit validates all snapshot versions
//    then writes back private pages and bumps page versions.
//    Abort discards private copies and releases ownership.
// ═══════════════════════════════════════════════════════════════

// ── Constants ──────────────────────────────────────────────────
constexpr size_t PAGE_SHIFT = 12;       // 4 KB pages
constexpr size_t PAGE_SIZE = 1ULL << PAGE_SHIFT; // 4096
constexpr uintptr_t PAGE_MASK = ~(PAGE_SIZE - 1);
constexpr size_t XADT_SIZE = 1ULL << 20; // ~1M entries ≈ 32 MB

// ── jmpbuf (defined in runtime) ────────────────────────────────
extern __thread sigjmp_buf *jmpbuf_ptr;

// ── XADT Entry ─────────────────────────────────────────────────
// Per-page metadata in the global transaction address/data table.
//   version    – incremented on every commit that touches this page
//   owner_tx_id – 0 = free, otherwise the tx id that owns the page
struct XADTEntry {
    std::atomic<uint64_t> version{0};
    std::atomic<uint64_t> owner_tx_id{0};
};

// ── XADT (global page table) ───────────────────────────────────
extern XADTEntry *g_xadt;  // dynamically allocated in init()

inline size_t xadt_index(void *addr) {
    return (((uintptr_t)addr) >> PAGE_SHIFT) & (XADT_SIZE - 1);
}

// ── XF (XADT Filter) — Bloom filter ──────────────────────────
// A simple bit-vector bloom filter for fast negative lookups.
// If the bit for an address is 0, the page is definitely not
// owned by anyone.  If 1, it *may* be owned.
constexpr size_t XF_BITS = 1ULL << 16; // 8 KB filter
extern std::atomic<uint8_t> g_xf[XF_BITS];

inline size_t xf_index(void *addr) {
    // Two hash functions for better distribution
    uintptr_t p = (uintptr_t)addr >> PAGE_SHIFT;
    return (p ^ (p >> 10)) & (XF_BITS - 1);
}

inline void xf_set(void *addr) {
    g_xf[xf_index(addr)].store(1, std::memory_order_relaxed);
}

inline bool xf_test(void *addr) {
    return g_xf[xf_index(addr)].load(std::memory_order_relaxed) != 0;
}

// ── Transaction control block (XSW + local logs) ─────────────
struct Transaction {
    uint64_t id = 0;
    bool    active = false;
    bool    read_only = true;
    int     abort_count = 0;
    bool    is_retry = false;

    // Write set:  page_addr → private_copy
    std::unordered_map<void *, void *> write_set;
    // Read set:   page_addr → snapshot_version
    std::unordered_map<void *, uint64_t> read_set;

    void reset() {
        active = false;
        read_only = true;
        abort_count = 0;
        is_retry = false;
        clear();
    }

    void clear() {
        write_set.clear();
        read_set.clear();
    }
};

// ── Globals ─────────────────────────────────────────────────────
extern std::atomic<uint64_t> g_tx_counter;
extern __thread Transaction *current_tx;
extern std::atomic<uint64_t> g_abort_counter;

// ── Init / Exit ─────────────────────────────────────────────────
inline void init() {
    g_tx_counter.store(1, std::memory_order_release);
    g_abort_counter.store(0, std::memory_order_release);

    // Allocate XADT
    g_xadt = new XADTEntry[XADT_SIZE]();
    for (size_t i = 0; i < XADT_SIZE; i++) {
        g_xadt[i].version.store(0, std::memory_order_release);
        g_xadt[i].owner_tx_id.store(0, std::memory_order_release);
    }

    // Clear bloom filter
    for (size_t i = 0; i < XF_BITS; i++)
        g_xf[i].store(0, std::memory_order_release);
}

inline void exit() {
    delete[] g_xadt;
    g_xadt = nullptr;
}

inline void init_thread() {
    if (!current_tx) {
        current_tx = new Transaction();
        current_tx->id = g_tx_counter.fetch_add(1, std::memory_order_acq_rel);
    }
    current_tx->reset();
}

inline void exit_thread() {
    delete current_tx;
    current_tx = nullptr;
}

// ── Private page allocator ──────────────────────────────────────
inline void *alloc_private_page() {
    void *p = nullptr;
    if (posix_memalign(&p, PAGE_SIZE, PAGE_SIZE) != 0) {
        fprintf(stderr, "XTM: posix_memalign failed\n");
        std::abort();
    }
    return p;
}

inline void free_private_page(void *p) {
    std::free(p);
}

// ── Begin ───────────────────────────────────────────────────────
inline bool begin() {
    auto *tx = current_tx;
    if (tx->active) return true;  // nested

    tx->clear();
    tx->active = true;
    tx->read_only = true;
    if (!tx->is_retry) tx->abort_count = 0;
    tx->is_retry = false;
    return true;
}

// ── Abort ───────────────────────────────────────────────────────
[[noreturn]] inline void abort_tx() {
    auto *tx = current_tx;

    // Release ownership and free private copies
    for (auto &kv : tx->write_set) {
        void *page = kv.first;
        void *priv = kv.second;
        size_t idx = xadt_index(page);
        g_xadt[idx].owner_tx_id.store(0, std::memory_order_release);
        free_private_page(priv);
    }

    tx->clear();
    tx->abort_count++;
    tx->is_retry = true;
    tx->active = false;
    stm::tm_token_release_if_held(tx->id);
    g_abort_counter.fetch_add(1, std::memory_order_relaxed);
    siglongjmp(*jmpbuf_ptr, 1);
}

// ── Commit ──────────────────────────────────────────────────────
// Phase 1: validate that no page in our read-set has had its
//           version change since we first read it.
// Phase 2: write back private copies, bump versions, release
//           ownership.
inline bool commit() {
    auto *tx = current_tx;
    if (!tx->active) return true;

    if (!tx->read_only) {
        // ── Validate read-set snapshots ────────────────────────
        for (auto &kv : tx->read_set) {
            void *page = kv.first;
            uint64_t snapshot = kv.second;
            size_t idx = xadt_index(page);

            uint64_t current_ver =
                g_xadt[idx].version.load(std::memory_order_acquire);
            if (current_ver != snapshot) {
                abort_tx();  // conflict detected
            }
        }

        // ── Write back private copies ──────────────────────────
        for (auto &kv : tx->write_set) {
            void *page = kv.first;
            void *priv = kv.second;

            // Copy the private page back to the original location
            std::memcpy(page, priv, PAGE_SIZE);

            size_t idx = xadt_index(page);
            // Bump version to invalidate any concurrent snapshots
            g_xadt[idx].version.fetch_add(1, std::memory_order_acq_rel);
            // Release ownership
            g_xadt[idx].owner_tx_id.store(0, std::memory_order_release);

            free_private_page(priv);
        }
    }

    stm::tm_token_release();
    tx->reset();
    return true;
}

// ── Read word ──────────────────────────────────────────────────
inline any_type_t read_word(Transaction *tx, void *addr, ValueType sz) {

    TM_ASSERT(tx && tx->active, "xtm read: no active tx");

    if (!stm::isTMAddress(addr)) {
        return read_value_from_addr(addr, sz);
    }

    void *page = (void *)((uintptr_t)addr & PAGE_MASK);
    size_t idx = xadt_index(page);

    // ── Ownership check (via XADT) ────────────────────────────
    uint64_t owner = g_xadt[idx].owner_tx_id.load(std::memory_order_acquire);
    if (owner != 0 && owner != tx->id) {
        // Page is owned by another transaction → conflict
        abort_tx();
    }

    // ── If we have a private copy, read from it ──────────────
    auto wit = tx->write_set.find(page);
    if (wit != tx->write_set.end()) {
        size_t offset = (uintptr_t)addr - (uintptr_t)page;
        return read_value_from_addr(
            (void *)((uintptr_t)wit->second + offset), sz);
    }

    // ── First read of this page — record a version snapshot ──
    //    (only if the page is NOT owned by someone else — we
    //     already checked that above, so owner == 0 or our id)
    uint64_t ver = g_xadt[idx].version.load(std::memory_order_acquire);
    tx->read_set.try_emplace(page, ver);

    return read_value_from_addr(addr, sz);
}

// ── Write word ─────────────────────────────────────────────────
inline void write_word(Transaction *tx, void *addr, any_type_t val,
                       ValueType sz) {

    TM_ASSERT(tx && tx->active, "xtm write: no active tx");

    if (!stm::isTMAddress(addr)) {
        write_value_to_addr(addr, val, sz);
        return;
    }

    tx->read_only = false;

    void *page = (void *)((uintptr_t)addr & PAGE_MASK);
    size_t idx = xadt_index(page);
    size_t offset = (uintptr_t)addr - (uintptr_t)page;

    // ── Acquire ownership of the page (CAS on XADT entry) ─────
    uint64_t expected = 0;
    if (!g_xadt[idx].owner_tx_id.compare_exchange_strong(
            expected, tx->id, std::memory_order_acq_rel)) {
        if (expected != tx->id) {
            // Page is owned by another transaction → conflict
            abort_tx();
        }
    } else {
        // We just acquired ownership — set the bloom filter bit
        xf_set(page);
    }

    // ── Get or create private copy ────────────────────────────
    auto wit = tx->write_set.find(page);
    if (wit == tx->write_set.end()) {
        void *priv = alloc_private_page();
        std::memcpy(priv, page, PAGE_SIZE);
        tx->write_set[page] = priv;
        wit = tx->write_set.find(page);
    }

    // Write to the private copy at the correct offset
    write_value_to_addr((void *)((uintptr_t)wit->second + offset), val, sz);
}

// ── Typed read/write templates ─────────────────────────────────
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

// ── Typed read/write wrappers ─────────────────────────────────
inline uint8_t  tm_read_i1(uint8_t  *addr) { return tm_read<uint8_t,  ValueType::UINT8>(addr);   }
inline uint16_t tm_read_i2(uint16_t *addr) { return tm_read<uint16_t, ValueType::UINT16>(addr);  }
inline uint32_t tm_read_i4(uint32_t *addr) { return tm_read<uint32_t, ValueType::UINT32>(addr);  }
inline uint64_t tm_read_i8(uint64_t *addr) { return tm_read<uint64_t, ValueType::UINT64>(addr);  }
inline float    tm_read_f4(float    *addr) { return tm_read<float,    ValueType::FLOAT>(addr);   }
inline double   tm_read_f8(double   *addr) { return tm_read<double,   ValueType::DOUBLE>(addr);  }
inline void *   tm_read_ptr(void   **addr) { return tm_read<void *,   ValueType::POINTER>(addr); }

inline void tm_write_i1(uint8_t  *addr, uint8_t  val) { tm_write<uint8_t,  ValueType::UINT8>(addr, val);   }
inline void tm_write_i2(uint16_t *addr, uint16_t val) { tm_write<uint16_t, ValueType::UINT16>(addr, val); }
inline void tm_write_i4(uint32_t *addr, uint32_t val) { tm_write<uint32_t, ValueType::UINT32>(addr, val); }
// tm_write_i8 accepts int64_t (no I8_CAST in the macro)
inline void tm_write_i8(uint64_t *addr, int64_t val) { tm_write<uint64_t, ValueType::UINT64>(addr, val); }
inline void tm_write_f4(float    *addr, float    val) { tm_write<float,    ValueType::FLOAT>(addr, val);   }
inline void tm_write_f8(double   *addr, double   val) { tm_write<double,   ValueType::DOUBLE>(addr, val);  }
inline void tm_write_ptr(void   **addr, void    *val) { tm_write<void *,   ValueType::POINTER>(addr, val); }

} // namespace xtm
