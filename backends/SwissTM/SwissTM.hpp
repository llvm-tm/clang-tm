/**
 * SwissTM_new - Full Implementation per Paper Specification
 * 
 * Features:
 * - Eager write/write conflict detection
 * - Lazy read/write conflict detection (validation at commit)
 * - Read locks with version numbers
 * - Global commit timestamp (commit_ts)
 * - Two-phase contention manager with random linear back-off
 * - Redo-logging scheme
 * - Multi-type support: 8, 16, 32, 64-bit integers, floats, doubles, pointers
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <cstring>
#include <csetjmp>
#include <random>
#include <thread>
#include <chrono>
#include <type_traits>

namespace swisstm {

using word_t = uintptr_t;

// Jump buffer for transaction retry (setjmp/longjmp)
thread_local sigjmp_buf tm_jmpbuf;
thread_local bool tm_jmpbuf_initialized = false;

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

constexpr unsigned OREC_TABLE_LOG_SIZE = 22;
constexpr unsigned OREC_TABLE_SIZE = 1 << OREC_TABLE_LOG_SIZE;
constexpr unsigned LOCK_EXTENT = 4;

constexpr word_t UNLOCKED = 0;
constexpr word_t READ_LOCKED = (word_t)-1;

constexpr int WN_THRESHOLD = 10;

enum class ValueType : uint8_t {
    UINT8, UINT16, UINT32, UINT64,
    FLOAT, DOUBLE, POINTER
};

union ValueData {
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f;
    double d;
    void* ptr;
    
    ValueData() : u64(0) {}
};

struct OwnershipRecord {
    std::atomic<word_t> r_lock;
    std::atomic<word_t> w_lock;
    uint8_t padding[48];
};

union ValueData;

struct OwnershipRecord;

struct TxDescriptor;

struct WriteLogEntry {
    void* byte_addr;
    word_t* word_addr;
    ValueData old_value;
    ValueData new_value;
    ValueType type;
    OwnershipRecord* orec;
    TxDescriptor* owner;
};

struct ReadLogEntry {
    void* byte_addr;
    word_t* word_addr;
    word_t version;
    ValueType type;
    OwnershipRecord* orec;
};

struct TxDescriptor {
    bool active = false;
    bool aborted = false;
    word_t valid_ts = 0;
    word_t cm_ts = 0;
    int write_count = 0;
    int succ_abort_count = 0;
    std::vector<WriteLogEntry> write_log;
    std::vector<OwnershipRecord*> write_set;
    std::vector<ReadLogEntry> read_set;
};

class STM {
private:
    static OwnershipRecord orec_table[OREC_TABLE_SIZE];
    static std::atomic<bool> initialized;
    static std::atomic<word_t> commit_ts;
    static std::atomic<word_t> greedy_ts;
    
    static thread_local std::mt19937 rng;
    static thread_local bool rng_initialized;
    
public:
    static OwnershipRecord* get_orec(word_t* addr) {
        unsigned idx = ((uintptr_t)addr >> LOCK_EXTENT) & (OREC_TABLE_SIZE - 1);
        return &orec_table[idx];
    }
    
    static void init() {
        if (!initialized.load(std::memory_order_seq_cst)) {
            for (auto& o : orec_table) {
                o.r_lock.store(UNLOCKED, std::memory_order_relaxed);
                o.w_lock.store(UNLOCKED, std::memory_order_relaxed);
            }
            commit_ts.store(0, std::memory_order_relaxed);
            greedy_ts.store(0, std::memory_order_relaxed);
            initialized.store(true);
        }
    }
    
    static word_t* get_word_addr(void* addr) {
        return reinterpret_cast<word_t*>((uintptr_t)addr & ~((uintptr_t)sizeof(word_t) - 1));
    }
    
    static bool is_locked(word_t lock_val) {
        return lock_val != UNLOCKED;
    }
    
    static bool is_locked_by(word_t lock_val, TxDescriptor* tx) {
        if (lock_val == UNLOCKED) return false;
        for (auto& we : tx->write_log) {
            if ((word_t)(&we) == lock_val) return true;
        }
        return false;
    }
    
    static void cm_start(TxDescriptor* tx) {
        if (tx->succ_abort_count == 0) {
            tx->cm_ts = (word_t)-1;
        }
    }
    
    static void cm_on_write(TxDescriptor* tx) {
        if (tx->cm_ts == (word_t)-1 && tx->write_count >= WN_THRESHOLD) {
            tx->cm_ts = greedy_ts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    static bool cm_should_abort(TxDescriptor* tx, OwnershipRecord* orec) {
        if (tx->cm_ts == (word_t)-1) {
            return true;
        }
        word_t owner = orec->w_lock.load(std::memory_order_acquire);
        if (owner != UNLOCKED) {
            WriteLogEntry* owner_entry = (WriteLogEntry*)owner;
            if (owner_entry->owner->cm_ts < tx->cm_ts) {
                return true;
            }
        }
        return false;
    }
    
    static void cm_on_rollback(TxDescriptor* tx) {
        if (!rng_initialized) {
            std::random_device rd;
            rng.seed(rd());
            rng_initialized = true;
        }
        std::uniform_int_distribution<> dist(1, 100 * (tx->succ_abort_count + 1));
        int delay = dist(rng);
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
    
    static bool validate(TxDescriptor* tx) {
        for (auto& re : tx->read_set) {
            word_t current_version = re.orec->r_lock.load(std::memory_order_acquire);
            if (current_version != re.version && !is_locked_by(re.orec->r_lock, tx)) {
                return false;
            }
        }
        return true;
    }
    
    static bool extend(TxDescriptor* tx) {
        word_t ts = commit_ts.load(std::memory_order_acquire);
        if (validate(tx)) {
            tx->valid_ts = ts;
            return true;
        }
        return false;
    }
    
    static void rollback(TxDescriptor* tx) {
        for (auto& we : tx->write_log) {
            we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
        }
        tx->aborted = true;
        tx->succ_abort_count++;
        cm_on_rollback(tx);
    }
    
    // ---- ValueData helpers ----
    template <typename T>
    static void set_value(ValueData& vd, T val) {
        if constexpr (std::is_same_v<T, uint8_t>) vd.u8 = val;
        else if constexpr (std::is_same_v<T, uint16_t>) vd.u16 = val;
        else if constexpr (std::is_same_v<T, uint32_t>) vd.u32 = val;
        else if constexpr (std::is_same_v<T, uint64_t>) vd.u64 = val;
        else if constexpr (std::is_same_v<T, float>) vd.f = val;
        else if constexpr (std::is_same_v<T, double>) vd.d = val;
        else vd.ptr = static_cast<void*>(val);
    }
    
    template <typename T>
    static T get_value(const ValueData& vd) {
        if constexpr (std::is_same_v<T, uint8_t>) return vd.u8;
        else if constexpr (std::is_same_v<T, uint16_t>) return vd.u16;
        else if constexpr (std::is_same_v<T, uint32_t>) return vd.u32;
        else if constexpr (std::is_same_v<T, uint64_t>) return vd.u64;
        else if constexpr (std::is_same_v<T, float>) return vd.f;
        else if constexpr (std::is_same_v<T, double>) return vd.d;
        else return static_cast<T>(vd.ptr);
    }
    
    // ---- Generic read implementation ----
    template <typename T, ValueType VT>
    static T read_impl(T* addr, TxDescriptor* tx) {
        if (!tx || !tx->active) return *addr;
        
        word_t* waddr = get_word_addr(addr);
        OwnershipRecord* orec = get_orec(waddr);
        
        for (auto& e : tx->write_log) {
            if (e.byte_addr == addr && e.type == VT) {
                return get_value<T>(e.new_value);
            }
        }
        
        word_t w_lock_val = orec->w_lock.load(std::memory_order_acquire);
        if (is_locked_by(w_lock_val, tx)) {
            return get_value<T>(((WriteLogEntry*)w_lock_val)->new_value);
        }
        
        word_t version;
        while (true) {
            version = orec->r_lock.load(std::memory_order_acquire);
            if (version == READ_LOCKED) {
                continue;
            }
            (void)*addr;
            word_t version2 = orec->r_lock.load(std::memory_order_acquire);
            if (version == version2) break;
            version = version2;
        }
        
        ReadLogEntry e;
        e.byte_addr = addr;
        e.word_addr = waddr;
        e.version = version;
        e.type = VT;
        e.orec = orec;
        tx->read_set.push_back(e);
        
        if (version > tx->valid_ts && !extend(tx)) {
            rollback(tx);
        }
        
        return *addr;
    }
    
    // Read functions
    static uint8_t read_u8(uint8_t* addr, TxDescriptor* tx) {
        return read_impl<uint8_t, ValueType::UINT8>(addr, tx);
    }
    static uint16_t read_u16(uint16_t* addr, TxDescriptor* tx) {
        return read_impl<uint16_t, ValueType::UINT16>(addr, tx);
    }
    static uint32_t read_u32(uint32_t* addr, TxDescriptor* tx) {
        return read_impl<uint32_t, ValueType::UINT32>(addr, tx);
    }
    static uint64_t read_u64(uint64_t* addr, TxDescriptor* tx) {
        return read_impl<uint64_t, ValueType::UINT64>(addr, tx);
    }
    static float read_float(float* addr, TxDescriptor* tx) {
        return read_impl<float, ValueType::FLOAT>(addr, tx);
    }
    static double read_double(double* addr, TxDescriptor* tx) {
        return read_impl<double, ValueType::DOUBLE>(addr, tx);
    }
    static void* read_ptr(void** addr, TxDescriptor* tx) {
        return read_impl<void*, ValueType::POINTER>(addr, tx);
    }
    
    // ---- Generic write implementation ----
    template <typename T, ValueType VT>
    static void write_impl(T* addr, T val, TxDescriptor* tx) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        if (!tx || !tx->active) { *addr = val; return; }
        
        word_t* waddr = get_word_addr(addr);
        OwnershipRecord* orec = get_orec(waddr);
        
        for (auto& e : tx->write_log) {
            if (e.byte_addr == addr && e.type == VT) {
                set_value(e.new_value, val);
                tx->write_count++;
                cm_on_write(tx);
                return;
            }
        }
        
        while (true) {
            word_t w_lock_val = orec->w_lock.load(std::memory_order_acquire);
            if (is_locked(w_lock_val)) {
                if (cm_should_abort(tx, orec)) {
                    rollback(tx);
                    return;
                }
                continue;
            }
            
            WriteLogEntry* log_entry = nullptr;
            for (auto& e : tx->write_log) {
                if (e.orec == orec) {
                    log_entry = &e;
                    break;
                }
            }
            
            if (!log_entry) {
                WriteLogEntry e;
                e.byte_addr = addr;
                e.word_addr = waddr;
                set_value(e.old_value, *addr);
                set_value(e.new_value, val);
                e.type = VT;
                e.orec = orec;
                e.owner = tx;
                tx->write_log.push_back(e);
                log_entry = &tx->write_log.back();
            }
            
            if (orec->w_lock.compare_exchange_strong(w_lock_val, (word_t)log_entry,
                    std::memory_order_acquire, std::memory_order_acquire)) {
                break;
            }
        }
        
        word_t r_lock_val = orec->r_lock.load(std::memory_order_acquire);
        if (r_lock_val > tx->valid_ts && !extend(tx)) {
            rollback(tx);
            return;
        }
        
        tx->write_count++;
        cm_on_write(tx);
    }
    
    // Write functions
    static void write_u8(uint8_t* addr, uint8_t val, TxDescriptor* tx) {
        write_impl<uint8_t, ValueType::UINT8>(addr, val, tx);
    }
    static void write_u16(uint16_t* addr, uint16_t val, TxDescriptor* tx) {
        write_impl<uint16_t, ValueType::UINT16>(addr, val, tx);
    }
    static void write_u32(uint32_t* addr, uint32_t val, TxDescriptor* tx) {
        write_impl<uint32_t, ValueType::UINT32>(addr, val, tx);
    }
    static void write_u64(uint64_t* addr, uint64_t val, TxDescriptor* tx) {
        write_impl<uint64_t, ValueType::UINT64>(addr, val, tx);
    }
    static void write_float(float* addr, float val, TxDescriptor* tx) {
        write_impl<float, ValueType::FLOAT>(addr, val, tx);
    }
    static void write_double(double* addr, double val, TxDescriptor* tx) {
        write_impl<double, ValueType::DOUBLE>(addr, val, tx);
    }
    static void write_ptr(void** addr, void* val, TxDescriptor* tx) {
        write_impl<void*, ValueType::POINTER>(addr, val, tx);
    }
    
    static void begin(TxDescriptor* tx) {
        if (!tx) return;
        tx->valid_ts = commit_ts.load(std::memory_order_acquire);
        tx->active = true;
        tx->aborted = false;
        tx->write_count = 0;
        tx->write_log.clear();
        tx->write_set.clear();
        tx->read_set.clear();
        cm_start(tx);
    }
    
    static void commit(TxDescriptor* tx) {
        if (!tx || !tx->active) return;
        
        if (tx->aborted) {
            for (auto& we : tx->write_log) {
                we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
            }
            tx->active = false;
            return;
        }
        
        if (tx->write_log.empty()) {
            tx->active = false;
            return;
        }
        
        for (auto& re : tx->read_set) {
            re.orec->r_lock.store(READ_LOCKED, std::memory_order_release);
        }
        
        word_t ts = commit_ts.fetch_add(1, std::memory_order_relaxed) + 1;
        
        if (ts > tx->valid_ts + 1 && !validate(tx)) {
            for (auto& re : tx->read_set) {
                re.orec->r_lock.store(re.version, std::memory_order_release);
            }
            for (auto& we : tx->write_log) {
                we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
            }
            rollback(tx);
            return;
        }
        
        for (auto& we : tx->write_log) {
            switch (we.type) {
                case ValueType::UINT8:
                    *((uint8_t*)we.byte_addr) = we.new_value.u8;
                    break;
                case ValueType::UINT16:
                    *((uint16_t*)we.byte_addr) = we.new_value.u16;
                    break;
                case ValueType::UINT32:
                    *((uint32_t*)we.byte_addr) = we.new_value.u32;
                    break;
                case ValueType::UINT64:
                    *((uint64_t*)we.byte_addr) = we.new_value.u64;
                    break;
                case ValueType::FLOAT:
                    *((float*)we.byte_addr) = we.new_value.f;
                    break;
                case ValueType::DOUBLE:
                    *((double*)we.byte_addr) = we.new_value.d;
                    break;
                case ValueType::POINTER:
                    *((void**)we.byte_addr) = we.new_value.ptr;
                    break;
            }
            we.orec->r_lock.store(ts, std::memory_order_release);
            we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
        }
        
	for (auto& re : tx->read_set) {
	    bool found = false;
	    for (auto& we : tx->write_log) {
	        if (we.orec == re.orec) { found = true; break; }
	    }
	    if (!found) {
	        re.orec->r_lock.store(re.version, std::memory_order_release);
	    }
	}

        tx->active = false;
    }
};

OwnershipRecord STM::orec_table[OREC_TABLE_SIZE];
std::atomic<bool> STM::initialized{false};
std::atomic<word_t> STM::commit_ts{0};
std::atomic<word_t> STM::greedy_ts{0};

thread_local std::mt19937 STM::rng;
thread_local bool STM::rng_initialized = false;

thread_local TxDescriptor* current_tx = nullptr;

inline void init() { STM::init(); }

inline void init_thread() {
    if (!current_tx) current_tx = new TxDescriptor();
}

inline void exit_thread() {
    if (current_tx) { delete current_tx; current_tx = nullptr; }
}

inline bool begin() { 
    init_thread(); 
    STM::begin(current_tx); 
    return true; 
}

inline void abort_tx() { 
    if (current_tx) current_tx->aborted = true; 
}

inline bool commit() {
    if (!current_tx || !current_tx->active) return false;
    STM::commit(current_tx);
    return !current_tx->aborted;
}

inline bool active() { return current_tx && current_tx->active; }
inline bool aborted() { return current_tx && current_tx->aborted; }

inline uint8_t tm_read_i1(uint8_t* addr) { return STM::read_u8(addr, current_tx); }
inline uint16_t tm_read_i2(uint16_t* addr) { return STM::read_u16(addr, current_tx); }
inline uint32_t tm_read_i4(uint32_t* addr) { return STM::read_u32(addr, current_tx); }
inline uint64_t tm_read_i8(uint64_t* addr) { return STM::read_u64(addr, current_tx); }
inline float tm_read_f4(float* addr) { return STM::read_float(addr, current_tx); }
inline double tm_read_f8(double* addr) { return STM::read_double(addr, current_tx); }
inline void* tm_read_ptr(void** addr) { return STM::read_ptr(addr, current_tx); }

inline void tm_write_i1(uint8_t* addr, uint8_t val) { STM::write_u8(addr, val, current_tx); }
inline void tm_write_i2(uint16_t* addr, uint16_t val) { STM::write_u16(addr, val, current_tx); }
inline void tm_write_i4(uint32_t* addr, uint32_t val) { STM::write_u32(addr, val, current_tx); }
inline void tm_write_i8(uint64_t* addr, uint64_t val) { STM::write_u64(addr, val, current_tx); }
inline void tm_write_f4(float* addr, float val) { STM::write_float(addr, val, current_tx); }
inline void tm_write_f8(double* addr, double val) { STM::write_double(addr, val, current_tx); }
inline void tm_write_ptr(void** addr, void* val) { STM::write_ptr(addr, val, current_tx); }

} // namespace swisstm