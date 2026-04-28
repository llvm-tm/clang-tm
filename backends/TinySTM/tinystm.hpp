/**
 * TinySTM_new - Simplified Word-Based STM
 * TL2-style: single global lock approach
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <csetjmp>
#include <thread>
#include <vector>

namespace tinystm {

constexpr const char* VERSION = "8.1.0-fix1";

using word_t = uintptr_t;

thread_local sigjmp_buf tm_jmpbuf;
thread_local bool tm_jmpbuf_initialized = false;

inline void set_jmp_env() {
    if (!tm_jmpbuf_initialized) {
        sigsetjmp(tm_jmpbuf, 0);
        tm_jmpbuf_initialized = true;
    }
}

// Copy jump buffer - needed for external integration with LLVM plugin
// Allows the runtime to use a jump buffer set up externally
inline void copy_jmp_env(sigjmp_buf* dest) {
    if (dest) {
        memcpy(dest, &tm_jmpbuf, sizeof(sigjmp_buf));
    }
}

// Set jump buffer from external source - allows abort to jump to external setjmp point
inline void set_jmp_env_external(const sigjmp_buf* src) {
    if (src) {
        memcpy(&tm_jmpbuf, src, sizeof(sigjmp_buf));
        tm_jmpbuf_initialized = true;
    }
}

enum class ValueType : uint8_t {
    UINT8 = 1, UINT16 = 2, UINT32 = 4, UINT64 = 8, POINTER = 0
};

inline word_t type_mask(ValueType t) {
    switch (t) {
        case ValueType::UINT8:  return 0xFF;
        case ValueType::UINT16: return 0xFFFF;
        case ValueType::UINT32: case ValueType::POINTER: return 0xFFFFFFFF;
        case ValueType::UINT64: return ~(word_t)0;
        default: return 0xFFFFFFFF;
    }
}

// Same word may have different byte offsets
struct ByteOffset {
    word_t base_addr;   // Word-aligned base address
    uint8_t offset;    // Byte offset within the word (0-7)
    
    ByteOffset() : base_addr(0), offset(0) {}
    ByteOffset(word_t addr) : base_addr(addr & ~7), offset(addr & 7) {}
};

inline bool same_location(const ByteOffset& a, const ByteOffset& b) {
    return a.base_addr == b.base_addr && a.offset == b.offset;
}

struct ReadLogEntry {
    ByteOffset location;
    word_t observed_word;  // Full word observed
    word_t observed_clock;
    ValueType type;
    
    word_t observed_value() const {
        uint8_t off = location.offset;
        switch (type) {
            case ValueType::UINT8:  
                return (observed_word >> (off * 8)) & 0xFF;
            case ValueType::UINT16: 
                return (observed_word >> (off * 8)) & 0xFFFF;
            default: return observed_word;
        }
    }
};

struct WriteLogEntry {
    ByteOffset location;
    word_t old_word;
    word_t new_word;
    ValueType type;
};

class Transaction {
public:
    word_t start_clock = 0;
    std::atomic<bool> active{false};
    std::atomic<bool> aborted{false};
    int nesting = 1;
    std::vector<ReadLogEntry> read_set;
    std::vector<WriteLogEntry> write_log;
    std::vector<word_t> locks_held;
    
    void reset() {
        start_clock = 0;
        active.store(false, std::memory_order_relaxed);
        aborted.store(false, std::memory_order_relaxed);
        nesting = 1;
        read_set.clear();
        write_log.clear();
        locks_held.clear();
    }
};

class Lock {
    std::atomic<word_t> state{0};
    
public:
    word_t get() const {
        return state.load(std::memory_order_acquire);
    }
    
    bool try_lock(word_t tx_id) {
        word_t expected = 0;
        return state.compare_exchange_strong(expected, tx_id,
                                            std::memory_order_acquire,
                                            std::memory_order_acquire);
    }
    
    void unlock() {
        state.store(0, std::memory_order_release);
    }
};

class LockTable {
    static constexpr size_t N = 8192;
    Lock locks[N];
    
public:
    Lock* get(word_t addr) {
        return &locks[(addr >> 3) & (N - 1)];
    }
};

static LockTable g_locks;
static std::atomic<word_t> g_clock{1};
thread_local Transaction* current_tx = nullptr;

inline word_t get_clock() {
    return g_clock.load(std::memory_order_relaxed);
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
    init_thread();
    
    auto* tx = current_tx;
    
    if (tx->active.load(std::memory_order_acquire)) {
        // Transaction is already active - this is a retry after abort
        if (tx->aborted.load(std::memory_order_acquire)) {
            tx->write_log.clear();
            tx->read_set.clear();
            tx->aborted.store(false, std::memory_order_relaxed);
            // DO NOT increment nesting - this is a retry, not a nested call
            // Update the start clock for the new attempt
            tx->start_clock = get_clock();
        } else {
            // This is a nested transaction call within the same tx
            tx->nesting++;
        }
        return true;
    }
    
    tx->reset();
    tx->start_clock = get_clock();
    tx->active.store(true, std::memory_order_release);
    
    return true;
}

inline void abort_tx() {
    auto* tx = current_tx;
    if (!tx) return;
    
    if (!tx->active.load(std::memory_order_acquire)) return;
    
    // Unlock any locks held by this transaction
    for (word_t addr : tx->locks_held) {
        g_locks.get(addr)->unlock();
    }
    tx->locks_held.clear();
    
    tx->aborted.store(true, std::memory_order_release);
    tx->write_log.clear();
    tx->read_set.clear();
    
    // Jump back to setjmp point if environment is set up
    if (tm_jmpbuf_initialized) {
        siglongjmp(tm_jmpbuf, 1);
    }
}

inline bool commit() {
    auto* tx = current_tx;
    if (!tx || !tx->active.load(std::memory_order_acquire)) return false;
    
    if (tx->nesting > 1) {
        tx->nesting--;
        return true;
    }
    
    if (tx->aborted.load(std::memory_order_acquire)) {
        abort_tx();
        tx->active.store(false, std::memory_order_release);
        return false;
    }
    
    word_t tx_id = (word_t)tx;
    
    for (auto& w : tx->write_log) {
        Lock* lock = g_locks.get(w.location.base_addr);
        if (!lock->try_lock(tx_id)) {
            abort_tx();
            tx->active.store(false, std::memory_order_release);
            return false;
        }
        tx->locks_held.push_back(w.location.base_addr);
    }
    
    for (auto& r : tx->read_set) {
        word_t current_word = *((volatile word_t*)r.location.base_addr);
        word_t mask = type_mask(r.type);
        // Shift mask to correct byte position
        uint8_t off = r.location.offset;
        if (off > 0) mask = mask << (off * 8);
        word_t changed = (current_word ^ r.observed_word) & mask;
        if (changed) {
            for (word_t addr : tx->locks_held) {
                g_locks.get(addr)->unlock();
            }
            tx->locks_held.clear();
            abort_tx();
            tx->active.store(false, std::memory_order_release);
            return false;
        }
    }
    
    for (auto& w : tx->write_log) {
        *((volatile word_t*)w.location.base_addr) = w.new_word;
    }
    tx->write_log.clear();
    tx->read_set.clear();
    
    for (word_t addr : tx->locks_held) {
        g_locks.get(addr)->unlock();
    }
    tx->locks_held.clear();
    
    g_clock.fetch_add(1, std::memory_order_relaxed);
    
    tx->active.store(false, std::memory_order_release);
    return true;
}

inline bool active() {
    return current_tx && current_tx->active.load(std::memory_order_acquire);
}

inline bool aborted() {
    return current_tx && current_tx->aborted.load(std::memory_order_acquire);
}

inline word_t read_word(word_t* addr, ValueType vt = ValueType::POINTER) {
    auto* tx = current_tx;
    if (!tx || !tx->active.load(std::memory_order_acquire)) return *addr;
    
    if (tx->aborted.load(std::memory_order_acquire)) {
        return *addr;
    }
    
    word_t base_addr = (word_t)addr;
    uint8_t offset = base_addr & 7;
    base_addr = base_addr & ~7;
    
    for (auto& w : tx->write_log) {
        if (w.location.base_addr == base_addr && 
            w.location.offset == offset && 
            w.type == vt) {
            // Extract the value from the stored word
            word_t word = w.new_word;
            switch (vt) {
                case ValueType::UINT8:
                    return (word >> (offset * 8)) & 0xFF;
                case ValueType::UINT16:
                    return (word >> (offset * 8)) & 0xFFFF;
                default:
                    return word;
            }
        }
    }
    
    ReadLogEntry e;
    e.location = ByteOffset((word_t)addr);
    e.observed_word = *((volatile word_t*)base_addr);
    e.observed_clock = tx->start_clock;
    e.type = vt;
    tx->read_set.push_back(e);
    
    // Return the specific bytes
    switch (vt) {
        case ValueType::UINT8:
            return (e.observed_word >> (offset * 8)) & 0xFF;
        case ValueType::UINT16:
            return (e.observed_word >> (offset * 8)) & 0xFFFF;
        default:
            return e.observed_word;
    }
}

inline void write_word(word_t* addr, word_t val, ValueType vt = ValueType::POINTER) {
    auto* tx = current_tx;
    if (!tx || !tx->active.load(std::memory_order_acquire)) { *addr = val; return; }
    
    if (tx->aborted.load(std::memory_order_acquire)) {
        return;
    }
    
    word_t base_addr = (word_t)addr;
    uint8_t offset = base_addr & 7;
    base_addr = base_addr & ~7;
    
    // First, try to merge with existing write to same location
    word_t mask;
    switch (vt) {
        case ValueType::UINT8: mask = 0xFF; break;
        case ValueType::UINT16: mask = 0xFFFF; break;
        default: mask = ~(word_t)0; break;
    }
    mask = mask << (offset * 8);
    
    for (auto& w : tx->write_log) {
        if (w.location.base_addr == base_addr && 
            w.location.offset == offset && 
            w.type == vt) {
            // Update existing entry
            w.new_word = (w.new_word & ~mask) | (val & mask);
            return;
        }
    }
    
    // Need to read the full word first
    WriteLogEntry e;
    e.location = ByteOffset((word_t)addr);
    e.old_word = *((volatile word_t*)base_addr);
    e.new_word = (e.old_word & ~mask) | (val & mask);
    e.type = vt;
    tx->write_log.push_back(e);
}

inline uint32_t read_u32(uint32_t* addr) {
    return (uint32_t)read_word((word_t*)addr, ValueType::UINT32);
}

inline void write_u32(uint32_t* addr, uint32_t val) {
    write_word((word_t*)addr, (word_t)val, ValueType::UINT32);
}

inline uint64_t read_u64(uint64_t* addr) {
    return (uint64_t)read_word((word_t*)addr, ValueType::UINT64);
}

inline void write_u64(uint64_t* addr, uint64_t val) {
    write_word((word_t*)addr, (word_t)val, ValueType::UINT64);
}

inline uint8_t read_u8(uint8_t* addr) {
    return (uint8_t)read_word((word_t*)addr, ValueType::UINT8);
}

inline void write_u8(uint8_t* addr, uint8_t val) {
    write_word((word_t*)addr, (word_t)val, ValueType::UINT8);
}

inline uint16_t read_u16(uint16_t* addr) {
    return (uint16_t)read_word((word_t*)addr, ValueType::UINT16);
}

inline void write_u16(uint16_t* addr, uint16_t val) {
    write_word((word_t*)addr, (word_t)val, ValueType::UINT16);
}

} // namespace tinystm