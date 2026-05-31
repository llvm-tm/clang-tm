#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#include <list>
#include <cstring>
#include <csetjmp>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <type_traits>
#include <cstdlib>
#include <unistd.h>
#include <pthread.h>

#include "../tm_common.hpp"
#include "../tm_spin_token.hpp"
#include "../tm_event_logger.hpp"

namespace swisstm
{

using word_t = uint64_t;

using stm::any_type_t;
using stm::isStackAddress;
using stm::ValueType;

#define TM_SPIN_BACKOFF() std::this_thread::sleep_for(std::chrono::microseconds(1))
using stm::read_value_from_addr;
using stm::write_value_to_addr;

extern __thread sigjmp_buf *jmpbuf;
inline std::atomic<uint64_t> g_tm_abort_count{0};

constexpr unsigned OREC_TABLE_LOG_SIZE = 22;
constexpr unsigned OREC_TABLE_SIZE = 1 << OREC_TABLE_LOG_SIZE;
constexpr unsigned LOCK_EXTENT = 4;

constexpr word_t UNLOCKED = 0;
constexpr word_t READ_LOCKED = (word_t)-1;

constexpr int WN_THRESHOLD = 10;

struct OwnershipRecord {
    std::atomic<word_t> r_lock;
    std::atomic<word_t> w_lock;
    uint8_t padding[48];
};

struct WriteLogEntry {
    void* byte_addr;
    word_t* word_addr;
    any_type_t old_value;
    any_type_t new_value;
    ValueType type;
    OwnershipRecord* orec;
    struct TxDescriptor* owner;
};

struct ReadLogEntry {
    OwnershipRecord* orec;
    void* byte_addr;
    word_t* word_addr;
    word_t version;
    word_t old_version; // pre-lock r_lock value captured at Phase 1
    ValueType type;
};

struct TxDescriptor {
    bool active = false;
    bool aborted = false;
    int64_t id = 0;
    word_t valid_ts = 0;
    word_t cm_ts = 0;
    int write_count = 0;
    int succ_abort_count = 0;
    std::list<WriteLogEntry> write_log;
    // Hash index mapping byte_addr → WriteLogEntry* for O(1) lookups
    std::unordered_map<void*, WriteLogEntry*> write_log_index;
    // Set of ORECs for which we already hold the w_lock
    std::unordered_set<OwnershipRecord*> owned_orecs;
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
            // Read owner's cm_ts atomically to avoid data race
            word_t owner_cm_ts = __atomic_load_n(&owner_entry->owner->cm_ts, __ATOMIC_ACQUIRE);
            if (owner_cm_ts < tx->cm_ts) {
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
            if (current_version != re.version) {
                if (is_locked_by(re.orec->r_lock, tx)) {
                    continue;
                }
                TM_EVENT2(READ_VERSION_CHECK, (word_t)re.byte_addr, re.version, current_version);
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
        TM_EVENT(TX_ABORT, tx->id, tx->succ_abort_count);
        for (auto& we : tx->write_log) {
            // Only restore old value and release w_lock if we actually
            // acquired it.  write_impl pushes the entry to write_log
            // BEFORE the CAS on w_lock.  If rollback is called from
            // cm_should_abort within the CAS loop, the entry has no
            // lock — restoring/releasing would corrupt another TX's
            // lock ownership and lose updates.
            if (tx->owned_orecs.find(we.orec) == tx->owned_orecs.end())
                continue;
            // Restore old value before releasing lock (SwissTM is
            // lazy-write-back: value in memory is still the original,
            // so restoring from undo-log is a no-op for the value,
            // but we must release the w_lock).
            switch (we.type) {
                case ValueType::UINT8:
                    *reinterpret_cast<uint8_t*>(we.byte_addr) = we.old_value.u1;
                    break;
                case ValueType::UINT16:
                    *reinterpret_cast<uint16_t*>(we.byte_addr) = we.old_value.u2;
                    break;
                case ValueType::UINT32:
                    *reinterpret_cast<uint32_t*>(we.byte_addr) = we.old_value.u4;
                    break;
                case ValueType::UINT64:
                    *reinterpret_cast<uint64_t*>(we.byte_addr) = we.old_value.u8;
                    break;
                case ValueType::FLOAT:
                    *reinterpret_cast<float*>(we.byte_addr) = we.old_value.f4;
                    break;
                case ValueType::DOUBLE:
                    *reinterpret_cast<double*>(we.byte_addr) = we.old_value.f8;
                    break;
                case ValueType::POINTER:
                    *reinterpret_cast<void**>(we.byte_addr) = we.old_value.ptr;
                    break;
            }
            we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
        }
        tx->aborted = true;
        tx->succ_abort_count++;
        g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
        stm::tm_token_release_if_held(tx->id);
        if (tx->succ_abort_count > 5) {
            cm_on_rollback(tx);
        }
        if (jmpbuf) {
            siglongjmp(*jmpbuf, 1);
        }
    }

    static void set_value(any_type_t& vd, uint8_t val) { vd.u1 = val; }
    static void set_value(any_type_t& vd, uint16_t val) { vd.u2 = val; }
    static void set_value(any_type_t& vd, uint32_t val) { vd.u4 = val; }
    static void set_value(any_type_t& vd, uint64_t val) { vd.u8 = val; }
    static void set_value(any_type_t& vd, float val) { vd.f4 = val; }
    static void set_value(any_type_t& vd, double val) { vd.f8 = val; }
    static void set_value(any_type_t& vd, void* val) { vd.ptr = val; }

    template <typename T>
    static T get_value(const any_type_t& vd);

    // ---- Generic read implementation ----
    template <typename T, ValueType VT>
    static T read_impl(T* addr, TxDescriptor* tx) {
        if (!tx || !tx->active) return *addr;

        word_t* waddr = get_word_addr(addr);
        OwnershipRecord* orec = get_orec(waddr);

	{
	    auto idx_it = tx->write_log_index.find(addr);
	    if (idx_it != tx->write_log_index.end()) {
	        WriteLogEntry* e = idx_it->second;
	        if (e->type == VT) {
	            if constexpr (std::is_same_v<T, uint8_t>) return e->new_value.u1;
	            else if constexpr (std::is_same_v<T, uint16_t>) return e->new_value.u2;
	            else if constexpr (std::is_same_v<T, uint32_t>) return e->new_value.u4;
	            else if constexpr (std::is_same_v<T, uint64_t>) return e->new_value.u8;
	            else if constexpr (std::is_same_v<T, float>) return e->new_value.f4;
	            else if constexpr (std::is_same_v<T, double>) return e->new_value.f8;
	            else return static_cast<T>(e->new_value.ptr);
	        }
	        // Type interchange: POINTER ↔ UINT64 (both 8 bytes, same union storage)
	        if constexpr (std::is_same_v<T, uint64_t>) {
	            if (e->type == ValueType::POINTER)
	                return reinterpret_cast<uint64_t>(e->new_value.ptr);
	        }
	        if constexpr (std::is_same_v<T, void*>) {
	            if (e->type == ValueType::UINT64)
	                return reinterpret_cast<void*>(e->new_value.u8);
	        }
	    }
	}

	word_t w_lock_val = orec->w_lock.load(std::memory_order_acquire);
	if (is_locked_by(w_lock_val, tx)) {
	    WriteLogEntry* log_entry = (WriteLogEntry*)w_lock_val;
	    if (log_entry->byte_addr == addr) {
	        if (log_entry->type == VT) {
	            any_type_t& nv = log_entry->new_value;
	            if constexpr (std::is_same_v<T, uint8_t>) return nv.u1;
	            else if constexpr (std::is_same_v<T, uint16_t>) return nv.u2;
	            else if constexpr (std::is_same_v<T, uint32_t>) return nv.u4;
	            else if constexpr (std::is_same_v<T, uint64_t>) return nv.u8;
	            else if constexpr (std::is_same_v<T, float>) return nv.f4;
	            else if constexpr (std::is_same_v<T, double>) return nv.f8;
	            else return static_cast<T>(nv.ptr);
	        }
	        // Type interchange: POINTER ↔ UINT64
	        if constexpr (std::is_same_v<T, uint64_t>) {
	            if (log_entry->type == ValueType::POINTER)
	                return reinterpret_cast<uint64_t>(log_entry->new_value.ptr);
	        }
	        if constexpr (std::is_same_v<T, void*>) {
	            if (log_entry->type == ValueType::UINT64)
	                return reinterpret_cast<void*>(log_entry->new_value.u8);
	        }
        }
    }

    // Byte-merge: scan write_log for entries whose address range
    // covers `addr`.  This handles the common case where the plugin
    // writes UINT64 via tm_write_i8 (memset/memmove expansion) and
    // a subsequent tm_read_i4/tm_read_ptr reads a sub-range.
    {
        uintptr_t r = (uintptr_t)addr;
        unsigned rsz = stm::type_size(VT);
        for (auto& w : tx->write_log) {
            uintptr_t wa = (uintptr_t)w.byte_addr;
            unsigned wsz = stm::type_size(w.type);
            if (r >= wa && r + rsz <= wa + wsz) {
                size_t off = r - wa;
                T result;
                memcpy(&result, (uint8_t*)&w.new_value + off, sizeof(T));
                return result;
            }
        }
    }

    word_t version;
        while (true) {
            version = orec->r_lock.load(std::memory_order_acquire);
            if (version == READ_LOCKED) {
                TM_SPIN_BACKOFF();
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
        TM_EVENT(READ_LOCK_ACQUIRE, (word_t)addr, version);

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
        // SwissTM eagerly reads *addr for the undo log.  The plugin never
        // generates null-address TM calls (verified via IR analysis), so
        // any null-address reaching here is a bug elsewhere, not something
        // to silently swallow.
        word_t* waddr = get_word_addr(addr);
        OwnershipRecord* orec = get_orec(waddr);

        unsigned sz_bytes = stm::type_size(VT);
        // Step 1: O(1) hash map lookup for existing (addr, type) entry
        {
            auto idx_it = tx->write_log_index.find(addr);
            if (idx_it != tx->write_log_index.end() && idx_it->second->type == VT) {
                set_value(idx_it->second->new_value, val);
                tx->write_count++;
                cm_on_write(tx);
                return;
            }
        }

        // Step 2: Check if a wider entry already covers this address
        {
            unsigned sz_bytes = stm::type_size(VT);
            auto idx_it = tx->write_log_index.find(addr);
            if (idx_it != tx->write_log_index.end() && idx_it->second->type != VT) {
                if (stm::type_size(idx_it->second->type) >= sz_bytes) {
                    tx->write_count++;
                    cm_on_write(tx);
                    return;
                }
            }
        }

        // Create write log entry
        WriteLogEntry e;
        e.byte_addr = addr;
        e.word_addr = waddr;
        // old_value filled after w_lock acquisition (ARM64 ordering fix)
        any_type_t new_val{};
        set_value(new_val, val);
        e.new_value = new_val;
        e.type = VT;
        e.orec = orec;
        e.owner = tx;
        tx->write_log.push_back(e);
        WriteLogEntry* log_entry = &tx->write_log.back();
        tx->write_log_index[addr] = log_entry;
        TM_EVENT2(WRITE_SET_INSERT, (word_t)addr, (word_t)VT, 0);

        // Step 4: Acquire w_lock (or skip if already held by us).
        if (tx->owned_orecs.find(orec) == tx->owned_orecs.end()) {
            word_t w_lock_val = orec->w_lock.load(std::memory_order_acquire);
            if (!is_locked_by(w_lock_val, tx)) {
                while (true) {
                    if (is_locked(w_lock_val)) {
                        if (stm::tm_token_soft_spin(tx->succ_abort_count, tx->id, 3)) {
                            while (is_locked(orec->w_lock.load(std::memory_order_relaxed))) {
                                stm::tm_cpu_relax();
                            }
                            w_lock_val = orec->w_lock.load(std::memory_order_acquire);
                            continue;
                        }
                        if (cm_should_abort(tx, orec)) {
                            rollback(tx);
                            return;
                        }
                        TM_SPIN_BACKOFF();
                        w_lock_val = orec->w_lock.load(std::memory_order_relaxed);
                        continue;
                    }

                    if (orec->w_lock.compare_exchange_strong(w_lock_val, (word_t)log_entry,
                            std::memory_order_acquire, std::memory_order_acquire)) {
                        TM_EVENT2(WRITE_LOCK_ACQUIRE, (word_t)addr, (word_t)orec, w_lock_val);
                        break;
                    }
                }
            }
            tx->owned_orecs.insert(orec);
        }

        // ARM64 ordering fix: fill old_value NOW (under w_lock) with
        // an acquire load.  A plain load (eager-read) could observe
        // stale data that was overwritten by a concurrent TX's Phase 4
        // write-back between the load and the acquire-CAS.  rollback()
        // restores old_value on abort; a stale old_value would
        // silently LOSE the concurrent TX's committed update.
        uint64_t locked_word = __atomic_load_n(waddr, __ATOMIC_ACQUIRE);
        size_t byte_off = (uintptr_t)addr & (sizeof(uint64_t) - 1);
        unsigned old_sz = stm::type_size(VT);
        memcpy(&log_entry->old_value, (const char*)&locked_word + byte_off, old_sz);

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
        tx->succ_abort_count = 0;
        tx->write_log.clear();
        tx->write_log_index.clear();
        tx->owned_orecs.clear();
        tx->write_set.clear();
        tx->read_set.clear();
        cm_start(tx);
        TM_EVENT(TX_BEGIN, tx->id, tx->valid_ts);
    }

    static void commit(TxDescriptor* tx) {
        if (!tx || !tx->active) return;

        if (tx->aborted) {
            for (auto& we : tx->write_log) {
                we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
            }
            stm::tm_token_release_if_held(tx->id);
            tx->active = false;
            return;
        }

        if (tx->write_log.empty()) {
            tx->active = false;
            stm::tm_token_release_if_held(tx->id);
            return;
        }

        // Phase 1: acquire read-locks via atomic exchange, capturing the
        // pre-lock version for Phase 3 validation.  Using a single atomic
        // exchange (not load-then-store) prevents two TXs from both seeing
        // old_version=0 and both passing Phase 3 — the second exchange
        // returns READ_LOCKED.
        //
        // Track already-locked orecs to handle duplicate read_set entries
        // (adjacent 4-byte values in the same 8-byte word map to the same
        // orec).  Without dedup, the second exchange returns READ_LOCKED
        // (our own store), giving old_version=READ_LOCKED ≠ re.version
        // → false abort → siglongjmp retry → infinite loop at 1+ threads.
        std::unordered_map<OwnershipRecord*, word_t> locked_orecs;
        for (auto& re : tx->read_set) {
            auto it = locked_orecs.find(re.orec);
            if (it != locked_orecs.end()) {
                re.old_version = it->second;
            } else {
                word_t old = re.orec->r_lock.exchange(
                    READ_LOCKED,
                    std::memory_order_acq_rel);
                re.old_version = old;
                locked_orecs[re.orec] = old;
                TM_EVENT2(COMMIT_LOCK_ACQUIRE, (word_t)re.byte_addr, (word_t)re.orec, old);
            }
        }

        word_t ts = commit_ts.fetch_add(1, std::memory_order_acq_rel) + 1;

        // Phase 3: validate read-set unconditionally.
        // Every orec with old_version != version means a concurrent TX
        // touched it: old_version==READ_LOCKED means another Phase-1
        // exchange acquired this orec after our read; old_version is a
        // commit timestamp means another TX committed and released.
        // In either case the value we read is stale and we must abort.
        //
        // CRITICAL: When old_version == READ_LOCKED, someone ELSE holds
        // the exchange lock — we must NOT release it in our abort path.
        // Doing so would overwrite the lock-holding TX's commit version
        // with 0, making subsequent TXs think no commit happened and
        // pass Phase 3 with stale values → lost updates.
        for (auto& re : tx->read_set) {
            if (re.old_version == re.version)
                continue;
                // Release only the orecs WE locked (old_version != READ_LOCKED).
                // Orecs with old_version == READ_LOCKED were locked by
                // another concurrent TX; leave them for that TX's Phase 5.
                for (auto& re2 : tx->read_set) {
                    if (re2.old_version == READ_LOCKED)
                        continue;
                    re2.orec->r_lock.store(
                        re2.old_version,
                        std::memory_order_release);
                }
                for (auto& we : tx->write_log) {
                    we.orec->w_lock.store(
                        UNLOCKED, std::memory_order_release);
                }
                rollback(tx);
                return;
            }

        // Phase 4: write-back all entries (no lock release yet).
        // Doing all write-backs before any lock release ensures that
        // concurrent readers see an atomic (per-OREC) snapshot when
        // the lock transitions from READ_LOCKED to ts.
        for (auto& we : tx->write_log) {
            TM_EVENT2(COMMIT_WRITEBACK, (word_t)we.byte_addr, (word_t)we.type, ts);
            write_value_to_addr(we.byte_addr, we.new_value, we.type);
        }

        // Phase 5: release locks (once per unique OREC, after all write-backs)
        {
            std::unordered_set<OwnershipRecord*> released_orecs;
            for (auto& we : tx->write_log) {
                if (released_orecs.find(we.orec) == released_orecs.end()) {
                    we.orec->r_lock.store(ts, std::memory_order_release);
                    we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
                    released_orecs.insert(we.orec);
                    TM_EVENT2(LOCK_RELEASE, (word_t)we.byte_addr, ts, 0);
                }
            }
        }

        // Release read-only orecs (not in write-log)
        for (auto& re : tx->read_set) {
            bool found = false;
            for (auto& we : tx->write_log) {
                if (we.orec == re.orec) { found = true; break; }
            }
            if (!found) {
                re.orec->r_lock.store(re.version, std::memory_order_release);
                TM_EVENT2(LOCK_RELEASE, (word_t)re.byte_addr, re.version, 0);
            }
        }

        stm::tm_token_release();
        tx->active = false;
        TM_EVENT(COMMIT_SUCCESS, 0, ts);
    }
};

OwnershipRecord STM::orec_table[OREC_TABLE_SIZE];
std::atomic<bool> STM::initialized{false};
std::atomic<word_t> STM::commit_ts{0};
std::atomic<word_t> STM::greedy_ts{0};

thread_local std::mt19937 STM::rng;
thread_local bool STM::rng_initialized = false;

thread_local TxDescriptor* current_tx = nullptr;
__thread sigjmp_buf *jmpbuf = nullptr;

inline void init() { STM::init(); }

inline void init_thread() {
    if (!current_tx) {
        current_tx = new TxDescriptor();
        current_tx->id = (int64_t)pthread_self();
    }
}

inline void exit_thread() {
    if (current_tx) { delete current_tx; current_tx = nullptr; }
}

inline void set_jmpbuf(sigjmp_buf *buf) { jmpbuf = buf; }

inline bool begin() {
    init_thread();
    STM::begin(current_tx);
    return true;
}

inline void abort_tx() {
    if (current_tx && !current_tx->aborted) {
        current_tx->aborted = true;
        STM::rollback(current_tx);
    }
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
