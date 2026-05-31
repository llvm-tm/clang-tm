#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <new>
#include <pthread.h>
#include <random>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../tm_common.hpp"
#include "../tm_debug.hpp"
#include "../tm_log_merge.hpp"
#include "../tm_spin_token.hpp"

namespace swisstm
{

using word_t = uint64_t;

using stm::any_type_t;
using stm::ValueType;

#define TM_SPIN_BACKOFF() std::this_thread::sleep_for(std::chrono::microseconds(1))
using stm::get_any_value;
using stm::read_value_from_addr;
using stm::write_value_to_addr;

extern __thread sigjmp_buf *jmpbuf;

// ── SwissTM-specific loss detection ─────────────────────────
// Used with DBG_EVT from tm_debug.hpp for counter_mt debugging.
// Set the counter pointer, then dbg_check_after_commit detects
// lost increments inline and aborts with log dump.
#ifndef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <atomic>
static std::atomic<uint64_t> g_dbg_stop{0};
static std::atomic<uint64_t> g_dbg_committed_ops{0};
static const volatile uint64_t *g_dbg_pcounter = nullptr;

static inline void dbg_set_counter_ptr(const volatile uint64_t *p) {
	g_dbg_pcounter = p;
}
static inline void dbg_check_after_commit() {
	if (!g_dbg_pcounter) return;
	uint64_t ops = g_dbg_committed_ops.fetch_add(1, std::memory_order_acq_rel) + 1;
	uint64_t actual = __atomic_load_n((const uint64_t *)g_dbg_pcounter, __ATOMIC_ACQUIRE);
	if (actual < ops) {
		tm_dbg_dump_all();
		g_dbg_stop.store(1, std::memory_order_release);
		fprintf(stderr, "\n=== LOST INCREMENT: committed=%lu counter=%lu ===\n",
		        (unsigned long)ops, (unsigned long)actual);
		_exit(1);
	}
}
#else
static inline void dbg_set_counter_ptr(const volatile uint64_t *) {}
static inline void dbg_check_after_commit() {}
#endif

constexpr unsigned OREC_TABLE_LOG_SIZE = 22;
constexpr unsigned OREC_TABLE_SIZE = 1 << OREC_TABLE_LOG_SIZE;
constexpr unsigned LOCK_EXTENT = 4;

constexpr word_t UNLOCKED = 0L;
constexpr word_t READ_LOCKED = (word_t)-1L;

constexpr int WN_THRESHOLD = 10;

struct OwnershipRecord {
	std::atomic<word_t> r_lock;
	std::atomic<word_t> w_lock;
	// uint8_t padding[48];
};

struct WriteLogEntry {
	void *byte_addr;
	word_t *word_addr;
	any_type_t old_value;
	any_type_t new_value;
	ValueType type;
	OwnershipRecord *orec;
	struct TxDescriptor *owner;
};

struct ReadLogEntry {
	OwnershipRecord *orec;
	void *byte_addr;
	word_t *word_addr;
	word_t version;
	word_t old_version; // pre-lock r_lock value captured at Phase 1
	ValueType type;
};

// ── Factory functions ──────────────────────────────────────────
inline ReadLogEntry make_read_entry(void *byte_addr,
                                    word_t *word_addr,
                                    word_t version,
                                    ValueType type,
                                    OwnershipRecord *orec)
{
	return {orec, byte_addr, word_addr, version, 0, type};
}

inline WriteLogEntry make_write_entry(void *byte_addr,
                                      word_t *word_addr,
                                      any_type_t old_val,
                                      any_type_t new_val,
                                      ValueType type,
                                      OwnershipRecord *orec,
                                      TxDescriptor *owner)
{
	return {byte_addr, word_addr, old_val, new_val, type, orec, owner};
}

struct TxDescriptor {
	bool active = false;
	bool aborted = false;
	int64_t id = 0;
	word_t valid_ts = 0;
	std::atomic<word_t> cm_ts = 0;
	int write_count = 0;
	int succ_abort_count = 0;
	std::list<WriteLogEntry> write_log;
	// Hash index mapping byte_addr → WriteLogEntry* for O(1) lookups
	std::unordered_map<void *, WriteLogEntry *> write_log_index;
	// Set of ORECs for which we already hold the w_lock
	std::unordered_set<OwnershipRecord *> owned_orecs;
	std::vector<OwnershipRecord *> write_set;
	std::vector<ReadLogEntry> read_set;
};

class STM
{
private:
	static OwnershipRecord orec_table[OREC_TABLE_SIZE];
	static std::atomic<bool> initialized;
	static std::atomic<word_t> commit_ts;
	static std::atomic<word_t> greedy_ts;
	static std::atomic<word_t> thr_counter;

public:
	static OwnershipRecord *get_orec(word_t *addr)
	{
		unsigned idx = ((uintptr_t)addr >> LOCK_EXTENT) & (OREC_TABLE_SIZE - 1);
		return &orec_table[idx];
	}

	static void init()
	{
		if (!initialized.load(std::memory_order_seq_cst)) {
			for (auto &o : orec_table) {
				o.r_lock.store(UNLOCKED, std::memory_order_release);
				o.w_lock.store(UNLOCKED, std::memory_order_release);
			}
			commit_ts.store(1, std::memory_order_release);
			greedy_ts.store(0, std::memory_order_release);
			initialized.store(true, std::memory_order_release);
		}
	}

	static word_t *get_word_addr(void *addr)
	{
		return reinterpret_cast<word_t *>(stm::merge::align_down_8(addr));
	}

	static bool is_locked(word_t lock_val) { return lock_val != UNLOCKED; }

	static bool is_locked_by(word_t lock_val, TxDescriptor *tx)
	{
		if (lock_val == UNLOCKED)
			return false;
		for (auto &we : tx->write_log) {
			if ((word_t)(&we) == lock_val)
				return true;
		}
		return false;
	}

	static void cm_start(TxDescriptor *tx)
	{
		if (tx->succ_abort_count == 0) {
			tx->cm_ts.store(READ_LOCKED, std::memory_order_release);
		}
	}

	static void cm_on_write(TxDescriptor *tx)
	{
		if (tx->cm_ts == READ_LOCKED && tx->write_count >= WN_THRESHOLD) {
			tx->cm_ts.store(greedy_ts.fetch_add(1, std::memory_order_relaxed),
			                std::memory_order_release);
		}
	}

	static bool cm_should_abort(TxDescriptor *tx, OwnershipRecord *orec)
	{
		if (tx->cm_ts == READ_LOCKED) {
			return true;
		}
		word_t owner = orec->w_lock.load(std::memory_order_acquire);
		if (owner != UNLOCKED) {
			WriteLogEntry *owner_entry = (WriteLogEntry *)owner;
			// Read owner's cm_ts atomically to avoid data race
			word_t owner_cm_ts = owner_entry->owner->cm_ts.load(
			    std::memory_order_acquire);
			if (owner_cm_ts < tx->cm_ts) {
				return true;
			}
		}
		return false;
	}

	static void cm_on_rollback(TxDescriptor *tx)
	{
		stm::random_backoff(tx->succ_abort_count);
	}

	static bool validate(TxDescriptor *tx)
	{
		for (auto &re : tx->read_set) {
			word_t current_version = re.orec->r_lock.load(std::memory_order_acquire);
			if (current_version != re.version && !is_locked_by(re.orec->r_lock, tx)) {
				return false;
			}
		}
		return true;
	}

	static bool extend(TxDescriptor *tx)
	{
		word_t ts = commit_ts.load(std::memory_order_acquire);
		if (validate(tx)) {
			tx->valid_ts = ts;
			return true;
		}
		return false;
	}

	static void rollback(TxDescriptor *tx)
	{
		DBG_EVT(3, tx->id);
		for (auto &we : tx->write_log) {
			// Only restore old value and release w_lock if we actually
			// acquired it.  write_impl pushes the entry to write_log
			// BEFORE the CAS on w_lock.  If rollback is called from
			// cm_should_abort within the CAS loop, the entry has no
			// lock — restoring/releasing would corrupt another TX's
			// lock ownership and lose updates.
			if (tx->owned_orecs.find(we.orec) == tx->owned_orecs.end())
				continue;
			// Atomically check ownership before restoring: if w_lock
			// still holds our log_entry pointer, release it and
			// restore the undo value.  If another TX has taken the
			// lock (committed) or memory differs from our undo
			// value, skip restore to avoid overwriting committed data.
			word_t expected = (word_t)&we;
			word_t actual_wl = we.orec->w_lock.load(std::memory_order_acquire);
			if (expected != actual_wl) {
				continue;
			}
			// Read current value from memory and compare to undo.
			// If they differ, another TX committed — skip restore.
			any_type_t mem_val = read_value_from_addr(
			    we.byte_addr, we.type);
			if (mem_val.u8 != we.old_value.u8) {
				we.orec->w_lock.store(UNLOCKED,
				                      std::memory_order_release);
				continue;
			}
			write_value_to_addr(we.byte_addr, we.old_value, we.type);
			we.orec->w_lock.store(UNLOCKED,
			                      std::memory_order_release);
		}
		tx->aborted = true;
		tx->succ_abort_count++;
		stm::tm_token_release_if_held(tx->id);
		if (tx->succ_abort_count > 5) {
			cm_on_rollback(tx);
		}
		if (jmpbuf) {
			siglongjmp(*jmpbuf, 1);
		}
	}

	template <typename T> static void set_value(any_type_t &vd, T val)
	{
		stm::any_type_mapping<T>::set(vd, val);
	}

	template <typename T> static T get_value(const any_type_t &vd);

	// ---- Generic read implementation ----
	template <typename T, ValueType VT> static T read_impl(T *addr, TxDescriptor *tx)
	{
		TM_ASSERT_VALID_TX(tx, "SwissTM read_impl");

		word_t *waddr = get_word_addr(addr);
		OwnershipRecord *orec = get_orec(waddr);

		{
			auto idx_it = tx->write_log_index.find(addr);
			if (idx_it != tx->write_log_index.end()) {
				WriteLogEntry *e = idx_it->second;
				if (e->type == VT)
					return get_any_value<T>(e->new_value);
				// Type interchange: POINTER ↔ UINT64 (both 8 bytes, same union storage)
				if constexpr (std::is_same_v<T, uint64_t>) {
					if (e->type == ValueType::POINTER)
						return reinterpret_cast<uint64_t>(e->new_value.ptr);
				}
				if constexpr (std::is_same_v<T, void *>) {
					if (e->type == ValueType::UINT64)
						return reinterpret_cast<void *>(e->new_value.u8);
				}
			}
		}

		word_t w_lock_val = orec->w_lock.load(std::memory_order_acquire);
		if (is_locked_by(w_lock_val, tx)) {
			WriteLogEntry *log_entry = (WriteLogEntry *)w_lock_val;
			if (log_entry->byte_addr == addr) {
				if (log_entry->type == VT)
					return get_any_value<T>(log_entry->new_value);
				// Type interchange: POINTER ↔ UINT64
				if constexpr (std::is_same_v<T, uint64_t>) {
					if (log_entry->type == ValueType::POINTER)
						return reinterpret_cast<uint64_t>(log_entry->new_value.ptr);
				}
				if constexpr (std::is_same_v<T, void *>) {
					if (log_entry->type == ValueType::UINT64)
						return reinterpret_cast<void *>(log_entry->new_value.u8);
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
			for (auto &w : tx->write_log) {
				uintptr_t wa = (uintptr_t)w.byte_addr;
				unsigned wsz = stm::type_size(w.type);
				if (r >= wa && r + rsz <= wa + wsz) {
					size_t off = r - wa;
					T result;
					memcpy(&result, (uint8_t *)&w.new_value + off, sizeof(T));
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
			if (version == version2)
				break;
			version = version2;
		}

		tx->read_set.push_back(make_read_entry(addr, waddr, version, VT, orec));

		if (version > tx->valid_ts && !extend(tx)) {
			rollback(tx);
		}

		DBG_EVT(0, *addr);
		return *addr;
	}

	// ── Try to update an existing write-log entry ─────────────────
	// Returns true if an entry at `addr` was found and updated.
	// Handles both same-type (direct overwrite) and different-type
	// (merge via stm::merge::same_address) cases.
	template <typename T, ValueType VT>
	static bool try_update_existing(T *addr, T val, TxDescriptor *tx)
	{
		auto idx_it = tx->write_log_index.find(addr);
		if (idx_it == tx->write_log_index.end())
			return false;

		WriteLogEntry *entry = idx_it->second;
		if (entry->type == VT) {
			set_value(entry->new_value, val);
			return true;
		}

		any_type_t wval{};
		set_value(wval, val);
		return stm::merge::same_address(entry->new_value, entry->type,
		                                wval, VT);
	}

	// ── Create a new write-log entry ─────────────────────────────
	// Pushes to write_log, populates write_log_index, and returns
	// a pointer to the stable entry (write_log is std::list).
	template <typename T, ValueType VT>
	static WriteLogEntry *create_write_log_entry(T *addr, T val,
	                                              word_t *waddr,
	                                              OwnershipRecord *orec,
	                                              TxDescriptor *tx)
	{
		any_type_t new_val{};
		set_value(new_val, val);
		any_type_t old_val = read_value_from_addr((void *)addr, VT);
		DBG_EVT(7, old_val.u8);
		tx->write_log.push_back(
		    make_write_entry(addr, waddr,
		                     old_val,
		                     new_val, VT, orec, tx));
		WriteLogEntry *log_entry = &tx->write_log.back();
		tx->write_log_index[addr] = log_entry;
		return log_entry;
	}

	// ── Acquire write-lock on an orec ────────────────────────────
	// Skips if the orec is already in owned_orecs (self-locked).
	// Uses token-based soft-spin on contention, with contention
	// manager fallback.  Calls rollback() if the contention manager
	// decides to abort.
	static void acquire_w_lock(OwnershipRecord *orec,
	                           WriteLogEntry *log_entry,
	                           TxDescriptor *tx)
	{
		if (tx->owned_orecs.find(orec) != tx->owned_orecs.end())
			return;

		word_t w_lock_val = orec->w_lock.load(std::memory_order_acquire);
		if (is_locked_by(w_lock_val, tx))
			return;

		while (true) {
			if (is_locked(w_lock_val)) {
				if (stm::tm_token_soft_spin(tx->succ_abort_count,
				                            tx->id, 3)) {
					while (is_locked(
					    orec->w_lock.load(std::memory_order_acquire))) {
						TINY_STM_PAUSE();
					}
					w_lock_val = orec->w_lock.load(std::memory_order_acquire);
					continue;
				}
				if (cm_should_abort(tx, orec)) {
					rollback(tx);
					return;
				}
				TM_SPIN_BACKOFF();
				w_lock_val = orec->w_lock.load(std::memory_order_acquire);
				continue;
			}

			if (orec->w_lock.compare_exchange_strong(w_lock_val,
			                                         (word_t)log_entry)) {
				break;
			}
		}
		tx->owned_orecs.insert(orec);
	}

	// ---- Generic write implementation ----
	template <typename T, ValueType VT>
	static void write_impl(T *addr, T val, TxDescriptor *tx)
	{
		std::atomic_signal_fence(std::memory_order_seq_cst);
		TM_ASSERT_VALID_TX(tx, "SwissTM write_impl");

		word_t *waddr = get_word_addr(addr);
		OwnershipRecord *orec = get_orec(waddr);

		if (try_update_existing<T, VT>(addr, val, tx)) {
			tx->write_count++;
			cm_on_write(tx);
			return;
		}

		WriteLogEntry *log_entry =
		    create_write_log_entry<T, VT>(addr, val, waddr, orec, tx);

		acquire_w_lock(orec, log_entry, tx);
		if (tx->aborted) return;

		DBG_EVT(1, val);

		word_t r_lock_val = orec->r_lock.load(std::memory_order_acquire);
		if (r_lock_val > tx->valid_ts && !extend(tx)) {
			rollback(tx);
			return;
		}

		tx->write_count++;
		cm_on_write(tx);
	}

	static void begin(TxDescriptor *tx)
	{
		TM_ASSERT(tx, "SwissTM begin: tx is null");

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
	}

	// ── Phase 1: acquire read-locks on all read-set orecs ──────────
	// Uses atomic exchange (not load-then-store) to prevent two TXs
	// from both seeing old_version=0 and both passing Phase 3.
	// Dedup by locked_orecs map handles adjacent same-orec entries.
	static void acquire_read_locks(
	    TxDescriptor *tx,
	    std::unordered_map<OwnershipRecord *, word_t> &locked_orecs)
	{
		for (auto &re : tx->read_set) {
			auto it = locked_orecs.find(re.orec);
			if (it != locked_orecs.end()) {
				re.old_version = it->second;
			} else {
				word_t old = re.orec->r_lock.exchange(
				    READ_LOCKED, std::memory_order_acq_rel);
				re.old_version = old;
				locked_orecs[re.orec] = old;
			}
		}
	}

	// ── Phase 3: validate read-set under read-locks ───────────────
	// Every orec with old_version != version means a concurrent TX
	// touched it.  On mismatch, releases self-locked orecs and calls
	// rollback().  Returns true if read-set is consistent.
	static bool validate_read_set(
	    TxDescriptor *tx,
	    const std::unordered_map<OwnershipRecord *, word_t> &locked_orecs)
	{
		for (auto &re : tx->read_set) {
			if (re.old_version == re.version)
				continue;
			for (auto &re2 : tx->read_set) {
				if (re2.old_version == READ_LOCKED)
					continue;
				re2.orec->r_lock.store(re2.old_version,
				                       std::memory_order_release);
			}
			// NOTE: w_lock release is handled inside rollback() via
			// CAS, so that the undo restore cannot race with another
			// TX that acquired the w_lock between Phase 3 and rollback.
			DBG_EVT(6, re.old_version);
			rollback(tx);
			return false;
		}
		DBG_EVT(5, tx->read_set.size());
		return true;
	}

	// ── Phase 4 + 5: write-back and release write-log orecs ───────
	static void write_back_and_release(TxDescriptor *tx, word_t ts)
	{
		for (auto &we : tx->write_log) {
			write_value_to_addr(we.byte_addr, we.new_value, we.type);
			we.orec->r_lock.store(ts, std::memory_order_release);
			we.orec->w_lock.store(UNLOCKED, std::memory_order_release);
		}
	}

	// ── Release read-set orecs not in write-log ───────────────────
	static void release_read_only_orecs(TxDescriptor *tx)
	{
		for (auto &re : tx->read_set) {
			bool found = false;
			for (auto &we : tx->write_log) {
				if (we.orec == re.orec) {
					found = true;
					break;
				}
			}
			if (!found) {
				re.orec->r_lock.store(re.version, std::memory_order_release);
			}
		}
	}

	static void commit(TxDescriptor *tx)
	{
		TM_ASSERT_VALID_TX(tx, "SwissTM commit");
		TM_ASSERT(!tx->aborted, "commit: stale aborted flag");

		if (tx->write_log.empty()) {
			tx->active = false;
			stm::tm_token_release_if_held(tx->id);
			return;
		}

		std::unordered_map<OwnershipRecord *, word_t> locked_orecs;
		acquire_read_locks(tx, locked_orecs);

		word_t ts = commit_ts.fetch_add(1, std::memory_order_acq_rel);

		if (!validate_read_set(tx, locked_orecs))
			return;

		write_back_and_release(tx, ts);

		release_read_only_orecs(tx);

		DBG_EVT(2, ts);
		dbg_check_after_commit();

		stm::tm_token_release();
		tx->active = false;
	}

	static word_t get_thr_id()
	{
		return thr_counter.fetch_add(1, std::memory_order_acq_rel);
	}
};

OwnershipRecord STM::orec_table[OREC_TABLE_SIZE];
std::atomic<bool> STM::initialized{false};
std::atomic<word_t> STM::commit_ts{1};
std::atomic<word_t> STM::greedy_ts{0};
std::atomic<word_t> STM::thr_counter{1};

thread_local TxDescriptor *current_tx = nullptr;
__thread sigjmp_buf *jmpbuf = nullptr;

inline void init() { STM::init(); }

inline void init_thread()
{
	if (!current_tx) {
		current_tx = new TxDescriptor();
		current_tx->id = STM::get_thr_id();
	}
}

inline void exit_thread()
{
	if (current_tx) {
		delete current_tx;
		current_tx = nullptr;
	}
}

inline void set_jmpbuf(sigjmp_buf *buf) { jmpbuf = buf; }

inline bool begin()
{
	init_thread();
	STM::begin(current_tx);
	return true;
}

inline void abort_tx()
{
	if (current_tx && !current_tx->aborted) {
		current_tx->aborted = true;
		STM::rollback(current_tx);
	}
}

inline bool commit()
{
	TM_ASSERT(current_tx, "commit: tx is null");
	TM_ASSERT(current_tx->active, "commit: tx not active");
	STM::commit(current_tx);
	return !current_tx->aborted;
}

inline bool active() { return current_tx && current_tx->active; }
inline bool aborted() { return current_tx && current_tx->aborted; }

// ── TM read/write stubs ──────────────────────────────────────
// SwissTM's read_impl/write_impl return/accept typed values directly
// (not any_type_t), so the shared tm_stubs.hpp (which expects an
// any_type_t-based read_word/write_word) cannot be used here.
// These local macros generate the 7 read + 7 write stub functions.

#define SWISS_READ_STUB(name, T, VT)                                                      \
	inline T tm_read_##name(T *addr)                                                       \
	{                                                                                      \
		return STM::read_impl<T, stm::ValueType::VT>(addr, current_tx);                    \
	}
#define SWISS_WRITE_STUB(name, T, VT)                                                     \
	inline void tm_write_##name(T *addr, T val)                                            \
	{                                                                                      \
		STM::write_impl<T, stm::ValueType::VT>(addr, val, current_tx);                     \
	}

SWISS_READ_STUB(i1, uint8_t,  UINT8)
SWISS_READ_STUB(i2, uint16_t, UINT16)
SWISS_READ_STUB(i4, uint32_t, UINT32)
SWISS_READ_STUB(i8, uint64_t, UINT64)
SWISS_READ_STUB(f4, float,    FLOAT)
SWISS_READ_STUB(f8, double,   DOUBLE)
SWISS_READ_STUB(ptr, void *,  POINTER)

SWISS_WRITE_STUB(i1, uint8_t,  UINT8)
SWISS_WRITE_STUB(i2, uint16_t, UINT16)
SWISS_WRITE_STUB(i4, uint32_t, UINT32)
SWISS_WRITE_STUB(i8, uint64_t, UINT64)
SWISS_WRITE_STUB(f4, float,    FLOAT)
SWISS_WRITE_STUB(f8, double,   DOUBLE)
SWISS_WRITE_STUB(ptr, void *,  POINTER)

#undef SWISS_READ_STUB
#undef SWISS_WRITE_STUB

} // namespace swisstm
