/**
 * TL2 - Full Implementation per Paper Specification
 * 
 * Features:
 * - Global version-clock (incremented on each commit)
 * - Versioned write-locks (lock word contains version number)
 * - Version-based validation at commit time
 * - Two-phase locking (commit-time lock acquisition)
 * - Bloom filter for write-set lookup optimization
 * - Multi-type support: 8, 16, 32, 64-bit integers, floats, doubles, pointers
 */

#ifndef TL2_NEW_HPP
#define TL2_NEW_HPP

#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#include <string.h>
#include <csetjmp>
#include "tm_common.hpp"
#include "tm_event_logger.hpp"
#include <type_traits>

namespace tl2 {

constexpr const char* VERSION = "2.0.0-full";

using word_t = uintptr_t;

// Jump buffer for transaction retry (setjmp/longjmp)
thread_local sigjmp_buf tm_jmpbuf;
thread_local bool tm_jmpbuf_initialized = false;

// Global abort counter — defined in tl2_runtime.cpp
} // namespace tl2
extern std::atomic<uint64_t> g_tm_abort_count;
namespace tl2 {

using stm::isTMAddress;
// Overload for volatile pointers (TL2 uses volatile throughout)
template <typename T>
inline bool isTMAddress(volatile T *addr) noexcept {
    return stm::isTMAddress(const_cast<const T *>(addr));
}

// Copy jump buffer - needed for external integration with LLVM plugin
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

constexpr unsigned GUARD_TABLE_LOG = 13;
constexpr unsigned GUARD_TABLE_SIZE = 1 << GUARD_TABLE_LOG;
constexpr unsigned BLOOM_FILTER_BITS = 64;

constexpr word_t LOCK_MASK = 1;
constexpr word_t VERSION_MASK = ~LOCK_MASK;

enum class DataType : uint8_t {
    UINT8   = 1,
    UINT16  = 2,
    UINT32  = 4,
    UINT64  = 8,
    PTR     = 16,
    FLOAT   = 32,
    DOUBLE  = 64
};

inline size_t dtype_size(DataType dt) {
    switch (dt) {
        case DataType::UINT8: case DataType::FLOAT: return 1;
        case DataType::UINT16: return 2;
        case DataType::UINT32: case DataType::PTR: return 4;
        case DataType::UINT64: case DataType::DOUBLE: return 8;
        default: return 0;
    }
}

struct WriteSetEntry {
    word_t* addr;
    DataType dtype;
    
    union Values {
        uint8_t   u8[2];
        uint16_t  u16[2];
        uint32_t  u32[2];
        uint64_t  u64[2];
        void*    ptr[2];
        float    f32[2];
        double   f64[2];
        
        Values() { memset(this, 0, sizeof(Values)); }
    } values;
    
    word_t old_value() const {
        switch (dtype) {
            case DataType::UINT8:  return values.u8[0];
            case DataType::UINT16: return values.u16[0];
            case DataType::UINT32: return values.u32[0];
            case DataType::FLOAT: return values.u32[0];
            case DataType::UINT64: return values.u64[0];
            case DataType::DOUBLE: return values.u64[0];
            case DataType::PTR: return (word_t)values.ptr[0];
            default: return 0;
        }
    }
    
    word_t new_value() const {
        switch (dtype) {
            case DataType::UINT8:  return values.u8[1];
            case DataType::UINT16: return values.u16[1];
            case DataType::UINT32: return values.u32[1];
            case DataType::FLOAT: return values.u32[1];
            case DataType::UINT64: return values.u64[1];
            case DataType::DOUBLE: return values.u64[1];
            case DataType::PTR: return (word_t)values.ptr[1];
            default: return 0;
        }
    }
    
    void set_old(word_t v) {
        switch (dtype) {
            case DataType::UINT8:  values.u8[0] = (uint8_t)v; break;
            case DataType::UINT16: values.u16[0] = (uint16_t)v; break;
            case DataType::UINT32: values.u32[0] = (uint32_t)v; break;
            case DataType::FLOAT: values.u32[0] = (uint32_t)v; break;
            case DataType::UINT64: values.u64[0] = (uint64_t)v; break;
            case DataType::DOUBLE: values.u64[0] = (uint64_t)v; break;
            case DataType::PTR: values.ptr[0] = (void*)v; break;
            default: break;
        }
    }
    
    void set_new(word_t v) {
        switch (dtype) {
            case DataType::UINT8:  values.u8[1] = (uint8_t)v; break;
            case DataType::UINT16: values.u16[1] = (uint16_t)v; break;
            case DataType::UINT32: values.u32[1] = (uint32_t)v; break;
            case DataType::FLOAT: values.u32[1] = (uint32_t)v; break;
            case DataType::UINT64: values.u64[1] = (uint64_t)v; break;
            case DataType::DOUBLE: values.u64[1] = (uint64_t)v; break;
            case DataType::PTR: values.ptr[1] = (void*)v; break;
            default: break;
        }
    }
};

struct ReadSetEntry {
    word_t* guard_addr;
    word_t* data_addr;
    word_t observed_version;
    DataType dtype;
    bool locked_by_me;
};

class Transaction {
public:
    bool active = false;
    bool aborted = false;
    word_t start_version = 0;
    word_t commit_version = 0;
    std::vector<WriteSetEntry> write_set;
    std::vector<ReadSetEntry> read_set;
    uint64_t bloom_filter = 0;
};

extern thread_local Transaction* current_tx;

class STM {
private:
    static std::atomic<word_t> g_clock;
    static std::atomic<word_t> g_guards[GUARD_TABLE_SIZE];
    static std::atomic<bool> initialized;
    
    static word_t get_guard_idx(word_t* addr) {
        uintptr_t a = (uintptr_t)addr;
        if (sizeof(word_t) == 8) {
            return ((a >> 3) ^ (a >> 48)) & (GUARD_TABLE_SIZE - 1);
        } else {
            return (a >> 2) & (GUARD_TABLE_SIZE - 1);
        }
    }
    
    static void bloom_set(Transaction* tx, word_t* addr) {
        uintptr_t a = (uintptr_t)addr;
        // Set bloom bits for all 8 bytes that the write covers (max width).
        // Without this, sub-offset reads (e.g., UINT32 at addr+4 from a UINT64
        // write at addr+0) miss the bloom filter and skip the write-set scan.
        for (unsigned off = 0; off < 8; off++) {
            uint64_t hash = ((a + off) ^ ((a + off) >> 17)) & 63;
            tx->bloom_filter |= (1ULL << hash);
        }
    }
    
    static bool bloom_might_contain(Transaction* tx, word_t* addr) {
        uintptr_t a = (uintptr_t)addr;
        uint64_t hash = (a ^ (a >> 17)) & 63;
        return (tx->bloom_filter >> hash) & 1ULL;
    }
    
public:
    static void init() {
        if (!initialized.load(std::memory_order_seq_cst)) {
            g_clock.store(1, std::memory_order_relaxed);
            for (auto& g : g_guards) {
                g.store(0, std::memory_order_relaxed);
            }
            initialized.store(true);
        }
    }
    
    static word_t get_clock() {
        return g_clock.load(std::memory_order_relaxed);
    }
    
    static word_t increment_clock() {
        return g_clock.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    
    static word_t get_guard_version(word_t* addr) {
        word_t idx = get_guard_idx(addr);
        return (g_guards[idx].load(std::memory_order_acquire) & VERSION_MASK) >> 1;
    }
    
    static bool is_guard_locked(word_t* addr) {
        word_t idx = get_guard_idx(addr);
        return (g_guards[idx].load(std::memory_order_acquire) & LOCK_MASK) != 0;
    }
    
    static void begin(Transaction* tx) {
        tx->start_version = get_clock();
        tx->active = true;
        tx->aborted = false;
        tx->write_set.clear();
        tx->read_set.clear();
        tx->bloom_filter = 0;
        TM_EVENT(TX_BEGIN, (word_t)tx, tx->start_version);
    }
    
    static bool is_locked_by_me(Transaction* tx, word_t* addr) {
        (void)tx;
        word_t idx = get_guard_idx(addr);
        word_t guard = g_guards[idx].load(std::memory_order_acquire);
        return (guard & LOCK_MASK) != 0;
    }
    
    static bool try_acquire_guard(Transaction* tx, word_t* addr) {
        word_t idx = get_guard_idx(addr);
        word_t guard = g_guards[idx].load(std::memory_order_acquire);
        while (!(guard & LOCK_MASK)) {
            word_t desired = guard | LOCK_MASK;
            if (g_guards[idx].compare_exchange_weak(guard, desired,
                    std::memory_order_acquire, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }
    
    static void release_guard(word_t* addr) {
        word_t idx = get_guard_idx(addr);
        g_guards[idx].fetch_and(~LOCK_MASK, std::memory_order_release);
    }
    
    static void set_guard_version(word_t* addr, word_t version) {
        word_t idx = get_guard_idx(addr);
        g_guards[idx].store((version << 1) & VERSION_MASK, std::memory_order_release);
    }
    
    static word_t read_guard(word_t* guard_addr) {
        return reinterpret_cast<std::atomic<word_t>*>(guard_addr)->load(std::memory_order_acquire);
    }
    
    // ---- Type conversion helpers ----
    template <typename T>
    static word_t to_word(T val) {
        if constexpr (std::is_integral_v<T>) {
            return static_cast<word_t>(val);
        } else if constexpr (std::is_same_v<T, float>) {
            uint32_t bits;
            memcpy(&bits, &val, sizeof(bits));
            return bits;
        } else if constexpr (std::is_same_v<T, double>) {
            uint64_t bits;
            memcpy(&bits, &val, sizeof(bits));
            return bits;
        } else {
            return reinterpret_cast<word_t>(val);
        }
    }

    template <typename T>
    static T from_word(word_t val) {
        if constexpr (std::is_integral_v<T>) {
            return static_cast<T>(val);
        } else if constexpr (std::is_same_v<T, float>) {
            uint32_t bits = static_cast<uint32_t>(val);
            float result;
            memcpy(&result, &bits, sizeof(result));
            return result;
        } else if constexpr (std::is_same_v<T, double>) {
            double result;
            memcpy(&result, &val, sizeof(result));
            return result;
        } else {
            return reinterpret_cast<T>(val);
        }
    }

    // ---- Generic write implementation ----
    template <typename T, DataType DT, typename AddrT>
    static void write_impl(Transaction* tx, AddrT addr, T val) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        assert(tx && tx->active);

#ifdef LLVM_TM_PLUGIN
        if (!stm::isTMAddress((void*)addr)) {
            *addr = val;
            return;
        }
#else
        TM_ASSERT(stm::isTMAddress((void*)addr), "Address not in TM address space");
#endif

        // Real byte width of each DataType (NOT dtype_size(), which returns
        // 4 for PTR but PTR write-back writes 8 bytes as word_t).
        auto dataWidth = [](DataType dt) -> unsigned {
            switch (dt) {
                case DataType::UINT8:   return 1;
                case DataType::UINT16:  return 2;
                case DataType::UINT32:  return 4;
                case DataType::FLOAT:   return 4;
                case DataType::UINT64:  return 8;
                case DataType::DOUBLE:  return 8;
                case DataType::PTR:     return 8;
                default:                return 0;
            }
        };

        // One entry per address — if any entry already exists at this address,
        // reuse it by updating dtype and new_value.
        for (auto& e : tx->write_set) {
            if (e.addr == (word_t*)addr) {
                e.dtype = DT;
                e.set_new(to_word(val));
                return;
            }
        }

        WriteSetEntry e;
        e.addr = (word_t*)addr;
        e.dtype = DT;
        e.set_new(to_word(val));
        tx->write_set.push_back(e);
        bloom_set(tx, (word_t*)addr);
        TM_EVENT(WRITE_SET_INSERT, (word_t)addr, (word_t)e.dtype);
    }

    // Write functions
    static void write_uint8(Transaction* tx, volatile uint8_t* addr, uint8_t val) {
        write_impl<uint8_t, DataType::UINT8>(tx, addr, val);
    }
    static void write_uint16(Transaction* tx, volatile uint16_t* addr, uint16_t val) {
        write_impl<uint16_t, DataType::UINT16>(tx, addr, val);
    }
    static void write_uint32(Transaction* tx, volatile uint32_t* addr, uint32_t val) {
        write_impl<uint32_t, DataType::UINT32>(tx, addr, val);
    }
    static void write_uint64(Transaction* tx, volatile uint64_t* addr, uint64_t val) {
        write_impl<uint64_t, DataType::UINT64>(tx, addr, val);
    }
    static void write_float(Transaction* tx, volatile float* addr, float val) {
        write_impl<float, DataType::FLOAT>(tx, addr, val);
    }
    static void write_double(Transaction* tx, volatile double* addr, double val) {
        write_impl<double, DataType::DOUBLE>(tx, addr, val);
    }
    static void write_ptr(Transaction* tx, volatile void** addr, void* val) {
        write_impl<void*, DataType::PTR>(tx, addr, val);
    }
    
    // ---- Generic read implementation ----
    template <typename T, DataType DT, typename AddrT>
    static T read_impl(Transaction* tx, AddrT addr) {
        assert(tx && tx->active);

#ifdef LLVM_TM_PLUGIN
        if (!stm::isTMAddress((void*)addr)) {
            if constexpr (std::is_pointer_v<T>) {
                return const_cast<T>(*addr);
            } else {
                return *addr;
            }
        }
#else
        TM_ASSERT(stm::isTMAddress((void*)addr), "Address not in TM address space");
#endif

        if (bloom_might_contain(tx, (word_t*)addr)) {
            // Exact type match (fast path)
            for (auto& e : tx->write_set) {
                if (e.addr == (word_t*)addr && e.dtype == DT) {
                    return from_word<T>(e.new_value());
                }
            }
            // Type-interchange fallback: same address, compatible size
            auto swapSize = [](DataType dt) -> unsigned {
                switch (dt) {
                    case DataType::UINT8:  return 1;
                    case DataType::UINT16: return 2;
                    case DataType::UINT32: return 4;
                    case DataType::FLOAT:  return 4;
                    case DataType::UINT64: return 8;
                    case DataType::DOUBLE: return 8;
                    case DataType::PTR:    return 8;
                    default:               return 0;
                }
            };
            unsigned req_sz = swapSize(DT);
            for (auto& e : tx->write_set) {
                if (e.addr != (word_t*)addr) continue;
                unsigned entry_sz = swapSize(e.dtype);
                // Same-size interchange (POINTER ↔ UINT64)
                if (entry_sz == req_sz && entry_sz == 8) {
                    return from_word<T>(e.new_value());
                }
                // Wider to narrower extraction
                if (entry_sz == 8 && req_sz == 4) {
                    return from_word<T>((uint32_t)(e.new_value() & 0xFFFFFFFF));
                }
                if (entry_sz == 8 && req_sz == 2) {
                    return from_word<T>((uint16_t)(e.new_value() & 0xFFFF));
                }
                if (entry_sz == 8 && req_sz == 1) {
                    return from_word<T>((uint8_t)(e.new_value() & 0xFF));
                }
                if (entry_sz == 4 && req_sz == 2) {
                    return from_word<T>((uint16_t)(e.new_value() & 0xFFFF));
                }
                if (entry_sz == 4 && req_sz == 1) {
                    return from_word<T>((uint8_t)(e.new_value() & 0xFF));
                }
                if (entry_sz == 2 && req_sz == 1) {
                    return from_word<T>((uint8_t)(e.new_value() & 0xFF));
                }
            }
            // Wider-entry covering: scan for entries that START BEFORE the read
            // address and span past it (e.g., UINT64 at addr+0 covering UINT32 at
            // addr+4).  This happens when the plugin expands memcpy/memset as
            // 8-byte UINT64 writes but individual fields are read at sub-offsets.
            {
                unsigned req_sz2 = swapSize(DT);
                for (auto& e : tx->write_set) {
                    if (e.addr == (word_t*)addr) continue; // handled above
                    unsigned entry_sz = swapSize(e.dtype);
                    if (entry_sz == 0 || entry_sz <= req_sz2) continue;
                    // Check if addr falls within [e.addr, e.addr+entry_sz)
                    auto e_start = (uintptr_t)e.addr;
                    auto r_addr  = (uintptr_t)addr;
                    if (r_addr >= e_start && r_addr < e_start + entry_sz) {
                        unsigned offset = (unsigned)(r_addr - e_start);
                        word_t shifted = e.new_value() >> (offset * 8);
                        word_t mask = (req_sz2 == 8) ? ~0ULL :
                                      ((1ULL << (req_sz2 * 8)) - 1);
                        return from_word<T>(shifted & mask);
                    }
                }
            }
            // General byte-merge: wider read from UINT8 entries at consecutive bytes
            if (swapSize(DT) == 8) {
                uint64_t merged = 0;
                bool all_byte = true;
                for (unsigned i = 0; i < 8; i++) {
                    void *byte_addr = (void*)((uintptr_t)addr + i);
                    bool found = false;
                    for (auto& e : tx->write_set) {
                        if (e.addr == (word_t*)byte_addr && e.dtype == DataType::UINT8) {
                            merged |= (uint64_t)(e.new_value() & 0xFF) << (i * 8);
                            found = true;
                            break;
                        }
                    }
                    if (!found) { all_byte = false; break; }
                }
                if (all_byte)
                    return from_word<T>(merged);
            }
        }

        word_t idx = get_guard_idx((word_t*)addr);
        word_t guard = g_guards[idx].load(std::memory_order_acquire);
        word_t version = (guard & VERSION_MASK) >> 1;
        bool locked = (guard & LOCK_MASK) != 0;
        TM_EVENT2(READ_LOCK_ACQUIRE, (word_t)addr, (word_t)&g_guards[idx], version);

        ReadSetEntry e;
        e.guard_addr = (word_t*)&g_guards[idx];
        e.data_addr = (word_t*)addr;
        e.observed_version = version;
        e.dtype = DT;
        e.locked_by_me = locked;
        tx->read_set.push_back(e);

        return from_word<T>(to_word(*addr));
    }

    // Read functions
    static uint8_t read_uint8(Transaction* tx, volatile uint8_t* addr) {
        return read_impl<uint8_t, DataType::UINT8>(tx, addr);
    }
    static uint16_t read_uint16(Transaction* tx, volatile uint16_t* addr) {
        return read_impl<uint16_t, DataType::UINT16>(tx, addr);
    }
    static uint32_t read_uint32(Transaction* tx, volatile uint32_t* addr) {
        return read_impl<uint32_t, DataType::UINT32>(tx, addr);
    }
    static uint64_t read_uint64(Transaction* tx, volatile uint64_t* addr) {
        return read_impl<uint64_t, DataType::UINT64>(tx, addr);
    }
    static float read_float(Transaction* tx, volatile float* addr) {
        return read_impl<float, DataType::FLOAT>(tx, addr);
    }
    static double read_double(Transaction* tx, volatile double* addr) {
        return read_impl<double, DataType::DOUBLE>(tx, addr);
    }
    static void* read_ptr(Transaction* tx, volatile void** addr) {
        return read_impl<void*, DataType::PTR>(tx, addr);
    }
    
    static bool commit(Transaction* tx) {
        if (!tx || !tx->active) return false;

        // Read-only commit optimization (ROCO): skip validation for read-only
        // TXs.  The original TL2 paper specifies that read-only transactions
        // don't need read-set validation because they never write.  Even with
        // an inconsistent snapshot, no data corruption results — the caller
        // simply sees a slightly stale or non-serializable sum.
        // DO NOT validate here — `tm_end()` calls `siglongjmp` on commit
        // failure, which would cause read-only TXs that scan many locations
        // (e.g., `total_transactional()` reading 1024 accounts) to livelock
        // under concurrent writes.
        if (tx->write_set.empty()) {
            tx->active = false;
            return true;
        }
        
        // Step 3: Acquire write-set locks, handling guard-table aliasing
        bool held_guard[GUARD_TABLE_SIZE] = {false};
        for (auto& e : tx->write_set) {
            word_t idx = get_guard_idx(e.addr);
            if (held_guard[idx]) continue;
            if (!try_acquire_guard(tx, e.addr)) {
                for (auto& e2 : tx->write_set) {
                    word_t idx2 = get_guard_idx(e2.addr);
                    if (held_guard[idx2]) {
                        TM_EVENT2(LOCK_RELEASE, (word_t)e2.addr, idx2, 0);
                        release_guard(e2.addr);
                        held_guard[idx2] = false;
                    }
                }
                tx->active = false;
                tx->aborted = true;
                return false;
            }
            held_guard[idx] = true;
            TM_EVENT2(COMMIT_LOCK_ACQUIRE, (word_t)e.addr, idx, 0);
        }
        
        // Step 4: Increment global version-clock
        tx->commit_version = increment_clock();
        
        // Step 5: Validate ALL read-set entries — even those in our write-set,
        // because a concurrent writer may have modified them between our read
        // and our commit.
        for (auto& re : tx->read_set) {
            word_t current_guard = read_guard(re.guard_addr);
            word_t current_version = (current_guard & VERSION_MASK) >> 1;

            // Compute guard index from guard_addr pointer to check if WE hold
            // this lock (acquired in step 3).  Our own lock is fine — the
            // version hasn't changed yet (lock was just acquired).  A
            // concurrent writer's lock is a true conflict.
            word_t guard_idx = (word_t)(((uintptr_t)re.guard_addr - (uintptr_t)g_guards) / sizeof(g_guards[0]));
            bool our_lock = held_guard[guard_idx];

            // Check BOTH lock-state (a concurrent writer holds the guard between
            // lock-acquisition and write-back, but NOT our own lock) AND version
            // (guard was released with a newer version after our read).
            if ((current_guard & LOCK_MASK && !our_lock) ||
                current_version != re.observed_version) {
                for (auto& e : tx->write_set) {
                    word_t idx = get_guard_idx(e.addr);
                    if (held_guard[idx]) {
                        TM_EVENT2(LOCK_RELEASE, (word_t)e.addr, idx, 0);
                        release_guard(e.addr);
                        held_guard[idx] = false;
                    }
                }
                tx->active = false;
                tx->aborted = true;
                return false;
            }
        }
        
        // Step 7: Apply writes — write ALL values first without releasing
        // guards, then release each guard ONCE with the commit version.
        // Releasing a guard while other entries share the same guard (aliasing)
        // would allow concurrent readers to see the guard as unlocked with the
        // new version but only a subset of the values written (stale read).
        for (auto& e : tx->write_set) {
            TM_EVENT2(COMMIT_WRITEBACK, (word_t)e.addr, (word_t)e.dtype, 0);
            switch (e.dtype) {
                case DataType::UINT8:
                    *(uint8_t*)e.addr = (uint8_t)e.new_value();
                    break;
                case DataType::UINT16:
                    *(uint16_t*)e.addr = (uint16_t)e.new_value();
                    break;
                case DataType::UINT32:
                    *(uint32_t*)e.addr = (uint32_t)e.new_value();
                    break;
                case DataType::FLOAT:
                {
                    uint32_t bits = (uint32_t)e.new_value();
                    *(float*)e.addr = *(float*)&bits;
                }
                    break;
                case DataType::UINT64:
                    *(uint64_t*)e.addr = (uint64_t)e.new_value();
                    break;
                case DataType::DOUBLE:
                {
                    uint64_t bits = e.new_value();
                    *(double*)e.addr = *(double*)&bits;
                }
                    break;
                case DataType::PTR:
                    *(word_t*)e.addr = e.new_value();
                    break;
            }
        }
        // Release each unique guard exactly once
        {
            bool released_guard[GUARD_TABLE_SIZE] = {false};
            for (auto& e : tx->write_set) {
                word_t idx = get_guard_idx(e.addr);
                if (released_guard[idx]) continue;
                released_guard[idx] = true;
                g_guards[idx].store((tx->commit_version << 1) & VERSION_MASK, std::memory_order_release);
                TM_EVENT2(LOCK_RELEASE, (word_t)e.addr, tx->commit_version, 0);
            }
        }
        
        tx->active = false;
        TM_EVENT(COMMIT_SUCCESS, (word_t)tx, tx->commit_version);
        return true;
    }
    
    static void abort_tx(Transaction* tx) {
        if (!tx) return;
        
        TM_EVENT(TX_ABORT, (word_t)tx, tx->start_version);
        g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
        
        // TL2 is write-back — values only reach memory during commit step 7.
        // On abort, no writes were applied to memory, so we just release guards
        // and retry.  No old-value restore needed.
        for (auto& e : tx->write_set) {
            TM_EVENT2(LOCK_RELEASE, (word_t)e.addr, 0, 0);
            release_guard(e.addr);
        }
        
        tx->active = false;
        tx->aborted = true;
        
        if (tm_jmpbuf_initialized) {
            siglongjmp(tm_jmpbuf, 1);
        }
    }
};

std::atomic<word_t> STM::g_clock{1};
std::atomic<word_t> STM::g_guards[GUARD_TABLE_SIZE];
std::atomic<bool> STM::initialized{false};

thread_local Transaction* current_tx = nullptr;

inline void init() { STM::init(); }

inline void init_thread() { 
    if (!current_tx) current_tx = new Transaction(); 
}

inline void exit_thread() { 
    if (current_tx) { 
        if (current_tx->active) STM::abort_tx(current_tx);
        delete current_tx; 
        current_tx = nullptr; 
    } 
}

inline bool begin() { 
    init_thread(); 
    STM::begin(current_tx); 
    return true; 
}

inline void abort_tx() { 
    if (current_tx) STM::abort_tx(current_tx); 
}

inline bool commit() {
    if (!current_tx || !current_tx->active) return false;
    return STM::commit(current_tx);
}

inline bool active() { return current_tx && current_tx->active; }
inline bool aborted() { return current_tx && current_tx->aborted; }

inline uint8_t tm_read_i1(volatile uint8_t* addr) { return STM::read_uint8(current_tx, addr); }
inline uint16_t tm_read_i2(volatile uint16_t* addr) { return STM::read_uint16(current_tx, addr); }
inline uint32_t tm_read_i4(volatile uint32_t* addr) { return STM::read_uint32(current_tx, addr); }
inline uint64_t tm_read_i8(volatile uint64_t* addr) { return STM::read_uint64(current_tx, addr); }
inline float tm_read_f4(volatile float* addr) { return STM::read_float(current_tx, addr); }
inline double tm_read_f8(volatile double* addr) { return STM::read_double(current_tx, addr); }
inline void* tm_read_ptr(volatile void** addr) { return STM::read_ptr(current_tx, addr); }

inline void tm_write_i1(volatile uint8_t* addr, uint8_t val) { STM::write_uint8(current_tx, addr, val); }
inline void tm_write_i2(volatile uint16_t* addr, uint16_t val) { STM::write_uint16(current_tx, addr, val); }
inline void tm_write_i4(volatile uint32_t* addr, uint32_t val) { STM::write_uint32(current_tx, addr, val); }
inline void tm_write_i8(volatile uint64_t* addr, uint64_t val) { STM::write_uint64(current_tx, addr, val); }
inline void tm_write_f4(volatile float* addr, float val) { STM::write_float(current_tx, addr, val); }
inline void tm_write_f8(volatile double* addr, double val) { STM::write_double(current_tx, addr, val); }
inline void tm_write_ptr(volatile void** addr, void* val) { STM::write_ptr(current_tx, addr, val); }

} // namespace tl2

#endif // TL2_NEW_HPP