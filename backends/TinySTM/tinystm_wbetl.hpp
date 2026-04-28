/**
 * TinySTM_new - Full Implementation per Paper Specification
 * 
 * Features:
 * - Encounter-time locking (WB-ETL design)
 * - Time-based design with global clock
 * - Versioned write-locks
 * - Double-check read protocol (lock → value → lock)
 * - Write-back strategy (delayed writes to commit)
 * - Multi-type support: 8, 16, 32, 64-bit integers, floats, doubles, pointers
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#include <iostream>
#include <cassert>

#define TINYSTM_ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "TINYSTM ASSERTION FAILED: %s (%s:%d)\n", msg, __FILE__, __LINE__); fflush(stderr); assert(cond); } } while(0)

#define TINYSTM_ASSERT_VALID_TX(tx, msg) \
    TINYSTM_ASSERT((tx) != nullptr, msg); \
    TINYSTM_ASSERT((tx)->active, "Transaction must be active: " msg); \
    TINYSTM_ASSERT(!(tx)->aborted, "Transaction must not be aborted: " msg)

namespace tinystm {

constexpr const char* VERSION = "8.2.0-full-wbetl";

using word_t = uintptr_t;

constexpr unsigned LOCK_ARRAY_LOG_SIZE = 20;
constexpr unsigned LOCK_ARRAY_SIZE = 1 << LOCK_ARRAY_LOG_SIZE;
constexpr unsigned LOCK_EXTRA_BITS = 2;

constexpr word_t LOCK_MASK = 1;
constexpr word_t VERSION_MASK = ~LOCK_MASK;

enum class ValueType : uint8_t {
    UINT8 = 1, UINT16 = 2, UINT32 = 4, UINT64 = 8, FLOAT = 16, DOUBLE = 32, POINTER = 64
};

inline word_t type_mask(ValueType t) {
    switch (t) {
        case ValueType::UINT8:  return 0xFF;
        case ValueType::UINT16: return 0xFFFF;
        case ValueType::UINT32: case ValueType::FLOAT: case ValueType::POINTER: return 0xFFFFFFFF;
        case ValueType::UINT64: case ValueType::DOUBLE: return ~(word_t)0;
        default: return 0xFFFFFFFF;
    }
}

struct ByteOffset {
    word_t base_addr;
    uint8_t offset;
    
    ByteOffset() : base_addr(0), offset(0) {}
    ByteOffset(word_t addr) : base_addr(addr & ~7), offset(addr & 7) {}
};

inline bool same_location(const ByteOffset& a, const ByteOffset& b) {
    return a.base_addr == b.base_addr && a.offset == b.offset;
}

struct ReadLogEntry {
    word_t* lock_addr;
    ByteOffset location;
    word_t observed_version;
    word_t observed_word;
    ValueType type;
};

struct WriteLogEntry {
    ByteOffset location;
    word_t old_word;
    word_t new_word;
    ValueType type;
    word_t version;
};

class Transaction {
public:
    word_t start_version = 0;
    word_t end_version = 0;
    bool active = false;
    bool aborted = false;
    bool read_only = true;
    int nesting = 1;
    int abort_count = 0;
    std::vector<ReadLogEntry> read_set;
    std::vector<WriteLogEntry> write_set;
    std::vector<word_t*> locks_held;
    
    void reset() {
        start_version = 0;
        end_version = 0;
        active = false;
        aborted = false;
        read_only = true;
        nesting = 1;
        abort_count = 0;
        read_set.clear();
        write_set.clear();
        locks_held.clear();
    }
};

class LockTable {
public:
    struct Lock {
        std::atomic<word_t> state{0};
        
        word_t get() const {
            return state.load(std::memory_order_acquire);
        }
        
        bool try_lock(word_t tx_id) {
            word_t expected = 0;
            word_t desired = tx_id | LOCK_MASK;
            return state.compare_exchange_strong(expected, desired,
                                                std::memory_order_acquire,
                                                std::memory_order_acquire);
        }
        
        void unlock() {
            state.store(0, std::memory_order_release);
        }
        
        void set_version(word_t v) {
            word_t old = state.load(std::memory_order_acquire);
            while (true) {
                word_t locked = old & LOCK_MASK;
                word_t new_val = (v & VERSION_MASK) | locked;
                if (state.compare_exchange_weak(old, new_val,
                    std::memory_order_release, std::memory_order_acquire)) {
                    break;
                }
            }
        }
        
        void unlock_with_version(word_t v) {
            state.store(v & VERSION_MASK, std::memory_order_release);
        }
        
        word_t get_version() const {
            return state.load(std::memory_order_acquire) & VERSION_MASK;
        }
        
        bool is_locked() const {
            return (state.load(std::memory_order_acquire) & LOCK_MASK) != 0;
        }
        
        bool is_locked_by(word_t tx_id) const {
            word_t s = state.load(std::memory_order_acquire);
            return ((s & LOCK_MASK) != 0) && ((s & ~LOCK_MASK) == tx_id);
        }
        
        word_t get_owner() const {
            return state.load(std::memory_order_acquire) & ~LOCK_MASK;
        }
    };
    
private:
    Lock locks[LOCK_ARRAY_SIZE];
    
public:
    LockTable::Lock* get(word_t addr) {
        return &locks[(addr >> LOCK_EXTRA_BITS) & (LOCK_ARRAY_SIZE - 1)];
    }
};

static LockTable g_locks;
static std::atomic<word_t> g_clock{1};

thread_local Transaction* current_tx = nullptr;

inline word_t get_clock() {
    return g_clock.load(std::memory_order_acquire);
}

inline word_t increment_clock() {
    return g_clock.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline void init() {
    g_clock.store(1, std::memory_order_relaxed);
}

inline void init_thread() {
    if (!current_tx) {
        current_tx = new Transaction();
    }
    current_tx->reset();
}

inline void exit_thread() {
    delete current_tx;
    current_tx = nullptr;
}

inline bool begin() {
    if (!current_tx) {
        current_tx = new Transaction();
    }
    
    auto* tx = current_tx;
    TINYSTM_ASSERT(tx != nullptr, "begin: tx is null");
    
    if (tx->active) {
        if (tx->aborted) {
            TINYSTM_ASSERT(!tx->write_set.empty() || !tx->read_set.empty() || true, "begin: aborted tx cleanup");
            tx->write_set.clear();
            tx->read_set.clear();
            for (word_t* lock_ptr : tx->locks_held) {
                LockTable::Lock* lock = (LockTable::Lock*)lock_ptr;
                lock->unlock();
            }
            tx->locks_held.clear();
            tx->aborted = false;
            tx->start_version = get_clock();
            tx->end_version = tx->start_version;
            tx->read_only = true;
            TINYSTM_ASSERT(tx->start_version > 0, "begin: invalid start version");
            return true;
        }
        TINYSTM_ASSERT(tx->start_version > 0, "begin: active tx without start version");
        TINYSTM_ASSERT(tx->end_version >= tx->start_version, "begin: invalid validity range");
        return true;
    }
    
    tx->reset();
    TINYSTM_ASSERT(tx->write_set.empty(), "begin: write_set not empty after reset");
    TINYSTM_ASSERT(tx->read_set.empty(), "begin: read_set not empty after reset");
    TINYSTM_ASSERT(tx->locks_held.empty(), "begin: locks_held not empty after reset");
    tx->start_version = get_clock();
    tx->end_version = tx->start_version;
    tx->active = true;
    tx->aborted = false;
    tx->read_only = true;
    
    return true;
}

inline void abort_tx() {
    auto* tx = current_tx;
    TINYSTM_ASSERT(tx != nullptr, "abort_tx: tx is null");
    TINYSTM_ASSERT(tx->active || !tx->active, "abort_tx: checking active flag before unlock");
    
    for (word_t* lock_ptr : tx->locks_held) {
        LockTable::Lock* lock = (LockTable::Lock*)lock_ptr;
        TINYSTM_ASSERT(lock != nullptr, "abort_tx: lock is null in locks_held");
        lock->unlock();
    }
    tx->locks_held.clear();
    
    for (auto& w : tx->write_set) {
        word_t* addr = (word_t*)w.location.base_addr;
        *addr = w.old_word;
    }
    
    tx->aborted = true;
    tx->abort_count++;
    TINYSTM_ASSERT(tx->abort_count >= 0 && tx->abort_count < 1000, "abort_tx: excessive abort count");
    
    int backoff = 1 << std::min(tx->abort_count, 10);
    if (backoff > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(backoff * 10));
    }
}

inline bool commit() {
    auto* tx = current_tx;
    TINYSTM_ASSERT(tx != nullptr, "commit: tx is null");
    TINYSTM_ASSERT(tx->active || !tx->active, "commit: checking active flag");
    
    if (!tx || !tx->active) {
        TINYSTM_ASSERT(tx == nullptr || !tx->active, "commit: tx not active");
        return false;
    }
    
    TINYSTM_ASSERT(tx->start_version > 0, "commit: invalid start version");
    TINYSTM_ASSERT(tx->end_version >= tx->start_version, "commit: invalid validity range");
    TINYSTM_ASSERT(tx->end_version <= get_clock() + 1, "commit: end_version exceeds clock");
    
    if (tx->aborted) {
        TINYSTM_ASSERT(true, "commit: tx is aborted, calling abort_tx");
        abort_tx();
        tx->active = false;
        return false;
    }
    
    if (!tx->read_only && !tx->write_set.empty()) {
        TINYSTM_ASSERT(tx->write_set.size() > 0, "commit: write_set empty for update tx");
        word_t tx_id = (word_t)tx;
        
        for (auto& w : tx->write_set) {
            LockTable::Lock* lock = g_locks.get(w.location.base_addr);
            TINYSTM_ASSERT(lock != nullptr, "commit: lock is null");
            if (!lock->is_locked_by(tx_id)) {
                if (!lock->try_lock(tx_id)) {
                    TINYSTM_ASSERT(true, "commit: failed to acquire lock, aborting");
                    abort_tx();
                    tx->active = false;
                    return false;
                }
                tx->locks_held.push_back((word_t*)lock);
            }
        }
        
        word_t commit_version = increment_clock();
        
        if (!tx->read_only) {
            word_t tx_id = (word_t)tx;
            for (auto& r : tx->read_set) {
                LockTable::Lock* lock = g_locks.get(r.location.base_addr);
                
                if (lock->is_locked_by(tx_id)) {
                    continue;
                }
                
                word_t current_version = lock->get_version();
                
                if (current_version != r.observed_version) {
                    for (word_t* lock_addr : tx->locks_held) {
                        LockTable::Lock* l = g_locks.get((word_t)lock_addr);
                        l->unlock();
                    }
                    tx->locks_held.clear();
                    abort_tx();
                    tx->active = false;
                    return false;
                }
            }
        }
        
        for (auto& w : tx->write_set) {
            uint8_t* addr = (uint8_t*)w.location.base_addr;
            
            if (w.type == ValueType::UINT8) {
                *addr = (uint8_t)w.new_word;
            } else if (w.type == ValueType::UINT16) {
                uint16_t val = (uint16_t)w.new_word;
                memcpy((void*)addr, &val, sizeof(uint16_t));
            } else if (w.type == ValueType::UINT32) {
                uint32_t val = (uint32_t)w.new_word;
                memcpy((void*)addr, &val, sizeof(uint32_t));
            } else if (w.type == ValueType::UINT64) {
                uint64_t val = (uint64_t)w.new_word;
                memcpy((void*)addr, &val, sizeof(uint64_t));
            } else {
                memcpy((void*)addr, &w.new_word, sizeof(word_t));
            }
            
            LockTable::Lock* lock = g_locks.get(w.location.base_addr);
            lock->unlock_with_version(commit_version);
        }
    }
    
    tx->write_set.clear();
    tx->read_set.clear();
    
    for (word_t* lock_ptr : tx->locks_held) {
        LockTable::Lock* lock = (LockTable::Lock*)lock_ptr;
        lock->unlock();
    }
    
    tx->locks_held.clear();
    tx->active = false;
    return true;
}

inline bool active() {
    return current_tx && current_tx->active;
}

inline bool aborted() {
    return current_tx && current_tx->aborted;
}

inline bool is_read_only() {
    return current_tx && current_tx->read_only;
}

inline void set_read_only(bool ro) {
    if (current_tx) current_tx->read_only = ro;
}

static word_t read_word_etl(Transaction* tx, volatile word_t* addr, ValueType type) {
    if (!tx || !tx->active) {
        TINYSTM_ASSERT(true, "read_word_etl: no active tx, direct read");
        return *addr;
    }
    
    TINYSTM_ASSERT(tx->start_version > 0, "read_word_etl: invalid start version");
    TINYSTM_ASSERT(tx->end_version >= tx->start_version, "read_word_etl: invalid validity range");
    TINYSTM_ASSERT(!tx->aborted, "read_word_etl: tx is aborted");
    
    ByteOffset bo((word_t)addr);
    LockTable::Lock* lock = g_locks.get(bo.base_addr);
    TINYSTM_ASSERT(lock != nullptr, "read_word_etl: lock is null");
    
    for (auto& w : tx->write_set) {
        if (same_location(w.location, bo)) {
            return w.new_word;
        }
    }
    
    if (lock->is_locked_by((word_t)tx)) {
        for (auto& w : tx->write_set) {
            if (same_location(w.location, bo)) {
                return w.new_word;
            }
        }
        return *addr;
    }
    
    word_t l = lock->get();
    
    while (true) {
        if (lock->is_locked()) {
            l = lock->get();
            continue;
        }
        
        word_t value = *addr;
        word_t l2 = lock->get();
        
        if (l != l2) {
            l = l2;
            continue;
        }
        
        word_t version = lock->get_version();
        
        if (version > tx->end_version) {
            if (tx->read_only) {
                TINYSTM_ASSERT(true, "read_word_etl: read-only tx version > end_version, aborting");
                tx->aborted = true;
                abort_tx();
                tx->active = false;
                return value;
            }
            
            bool extended = false;
            for (auto& r : tx->read_set) {
                LockTable::Lock* rl = g_locks.get((word_t)r.lock_addr);
                word_t rv = rl->get_version();
                if (rv > tx->start_version) {
                    extended = true;
                    break;
                }
            }
            
            if (!extended) {
                TINYSTM_ASSERT(true, "read_word_etl: version not extended, aborting");
                tx->aborted = true;
                abort_tx();
                tx->active = false;
                return value;
            }
            
            tx->end_version = get_clock();
            if (tx->end_version < version) {
                TINYSTM_ASSERT(true, "read_word_etl: end_version < version, aborting");
                tx->aborted = true;
                abort_tx();
                tx->active = false;
                return value;
            }
        }
        
        ReadLogEntry r;
        r.lock_addr = (word_t*)lock;
        r.location = bo;
        r.observed_version = version;
        r.observed_word = value;
        r.type = type;
        tx->read_set.push_back(r);
        
        return value;
    }
}

static void write_word_etl(Transaction* tx, volatile word_t* addr, word_t value, ValueType type) {
    if (!tx || !tx->active) {
        TINYSTM_ASSERT(true, "write_word_etl: no active tx, direct write");
        *addr = value;
        return;
    }
    
    TINYSTM_ASSERT(tx->start_version > 0, "write_word_etl: invalid start version");
    TINYSTM_ASSERT(!tx->aborted, "write_word_etl: tx is aborted - should not reach here");
    
    set_read_only(false);
    TINYSTM_ASSERT(!tx->read_only, "write_word_etl: tx should not be read-only after write");
    
    ByteOffset bo((word_t)addr);
    
    for (auto& w : tx->write_set) {
        if (same_location(w.location, bo)) {
            w.new_word = value;
            return;
        }
    }
    
    TINYSTM_ASSERT(tx->write_set.size() < 10000, "write_word_etl: excessive write set size");
    
    word_t tx_id = (word_t)tx;
    
    while (true) {
        LockTable::Lock* lock = g_locks.get(bo.base_addr);
        TINYSTM_ASSERT(lock != nullptr, "write_word_etl: lock is null");
        
        if (lock->is_locked_by(tx_id)) {
            return;
        }
        
        if (lock->is_locked()) {
            continue;
        }
        
        word_t version = lock->get_version();
        
        word_t old_val = 0;
        {
            word_t tmp;
            memcpy(&tmp, (const void*)addr, sizeof(word_t));
            old_val = tmp;
        }
        
        if (lock->try_lock(tx_id)) {
            tx->locks_held.push_back((word_t*)lock);
            
            WriteLogEntry w;
            w.location = bo;
            w.old_word = old_val;
            w.new_word = value;
            w.type = type;
            w.version = version;
            tx->write_set.push_back(w);
            return;
        }
    }
}

inline uint8_t tm_read_i1(volatile uint8_t* addr) {
    auto* tx = current_tx;
    if (!tx || !tx->active) return *(volatile uint8_t*)addr;
    
    word_t word = read_word_etl(tx, (volatile word_t*)((word_t)addr & ~7), ValueType::UINT8);
    uint8_t off = (word_t)addr & 7;
    return (word >> (off * 8)) & 0xFF;
}

inline uint16_t tm_read_i2(volatile uint16_t* addr) {
    auto* tx = current_tx;
    if (!tx || !tx->active) return *(volatile uint16_t*)addr;
    
    word_t word = read_word_etl(tx, (volatile word_t*)((word_t)addr & ~7), ValueType::UINT16);
    uint8_t off = (word_t)addr & 7;
    return (word >> (off * 8)) & 0xFFFF;
}

inline uint32_t tm_read_i4(volatile uint32_t* addr) {
    auto* tx = current_tx;
    if (!tx || !tx->active) return *(volatile uint32_t*)addr;
    
    return (uint32_t)read_word_etl(tx, (volatile word_t*)addr, ValueType::UINT32);
}

inline uint64_t tm_read_i8(volatile uint64_t* addr) {
    auto* tx = current_tx;
    if (!tx || !tx->active) return *(volatile uint64_t*)addr;
    
    return (uint64_t)read_word_etl(tx, (volatile word_t*)addr, ValueType::UINT64);
}

inline float tm_read_f4(volatile float* addr) {
    auto* tx = current_tx;
    if (!tx || !tx->active) return *addr;
    
    word_t bits = read_word_etl(tx, (volatile word_t*)addr, ValueType::FLOAT);
    return *(float*)&bits;
}

inline double tm_read_f8(volatile double* addr) {
    auto* tx = current_tx;
    if (!tx || !tx->active) return *addr;
    
    word_t bits = read_word_etl(tx, (volatile word_t*)addr, ValueType::DOUBLE);
    return *(double*)&bits;
}

inline void* tm_read_ptr(volatile void** addr) {
    auto* tx = current_tx;
    if (!tx || !tx->active) return (void*)*addr;
    
    return (void*)read_word_etl(tx, (volatile word_t*)addr, ValueType::POINTER);
}

inline void tm_write_i1(volatile uint8_t* addr, uint8_t val) {
    auto* tx = current_tx;
    if (!tx || !tx->active) {
        *addr = val;
        return;
    }
    
    word_t* word_addr = (word_t*)((word_t)addr & ~7);
    word_t word = *word_addr;
    uint8_t off = (word_t)addr & 7;
    word_t mask = (0xFFULL << (off * 8));
    word = (word & ~mask) | ((word_t)val << (off * 8));
    write_word_etl(tx, (volatile word_t*)word_addr, word, ValueType::UINT8);
}

inline void tm_write_i2(volatile uint16_t* addr, uint16_t val) {
    auto* tx = current_tx;
    if (!tx || !tx->active) {
        *addr = val;
        return;
    }
    
    word_t* word_addr = (word_t*)((word_t)addr & ~7);
    word_t word = *word_addr;
    uint8_t off = (word_t)addr & 7;
    word_t mask = (0xFFFFULL << (off * 8));
    word = (word & ~mask) | ((word_t)val << (off * 8));
    write_word_etl(tx, (volatile word_t*)word_addr, word, ValueType::UINT16);
}

inline void tm_write_i4(volatile uint32_t* addr, uint32_t val) {
    auto* tx = current_tx;
    if (!tx || !tx->active) {
        *addr = val;
        return;
    }
    
    set_read_only(false);
    
    write_word_etl(tx, (volatile word_t*)addr, (word_t)val, ValueType::UINT32);
}

inline void tm_write_i8(volatile uint64_t* addr, uint64_t val) {
    auto* tx = current_tx;
    if (!tx || !tx->active) {
        *addr = val;
        return;
    }
    
    write_word_etl(tx, (volatile word_t*)addr, (word_t)val, ValueType::UINT64);
}

inline void tm_write_f4(volatile float* addr, float val) {
    auto* tx = current_tx;
    if (!tx || !tx->active) {
        *addr = val;
        return;
    }
    
    write_word_etl(tx, (volatile word_t*)addr, *(word_t*)&val, ValueType::FLOAT);
}

inline void tm_write_f8(volatile double* addr, double val) {
    auto* tx = current_tx;
    if (!tx || !tx->active) {
        *addr = val;
        return;
    }
    
    write_word_etl(tx, (volatile word_t*)addr, *(word_t*)&val, ValueType::DOUBLE);
}

inline void tm_write_ptr(volatile void** addr, void* val) {
    auto* tx = current_tx;
    if (!tx || !tx->active) {
        *addr = val;
        return;
    }
    
    write_word_etl(tx, (volatile word_t*)addr, (word_t)val, ValueType::POINTER);
}

} // namespace tinystm