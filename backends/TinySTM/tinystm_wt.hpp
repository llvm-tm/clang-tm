/**
 * TinySTM - WRITE_THROUGH per Paper Specification
 *
 * Features:
 * - Encounter-time locking (lock on first write)
 * - Write-through strategy (direct writes to memory)
 * - Incarnation numbers for abort detection
 * - Double-check read protocol (lock → value → lock)
 * - Implements tinySTM's WT algorithm using the shared tinystm infrastructure
 */

#pragma once

#include <cassert>
#include <csetjmp>
#include <csignal>
#include <random>

#include "../tm_spin_token.hpp"
#include "../tm_event_logger.hpp"
#include "../tm_platform.hpp"
#include "tinystm_common.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
}

// ── Token fairness threshold ──────────────────────────────────
// After this many aborts, threads start checking whether the
// token is free before attempting CAS.  Below the threshold
// they abort immediately without touching the token at all.
constexpr int TOKEN_SOFT_SPIN_THRESHOLD = 3;

namespace tinystm
{

constexpr const char *VERSION = "0.3.0-wt";

/** -------------------------------------------------------
  * Log entries.
  * Both read- and write-sets are keyed by the aligned
  * (8-byte) address.  All stored values are full 8-byte
  * words — sub-word extraction/merging happens in the
  * tm_read_i1/tm_write_i1 wrappers.
  * ---------------------------------------------------- */

struct ReadLogEntry_wt {
	volatile word_t observed_version;
	word_t observed_incarnation;
	any_type_t observed_val;
};

struct WriteLogEntry_wt {
	any_type_t old_val;
	any_type_t new_val;
	word_t version;     // version at lock-acquisition time
	word_t incarnation; // incarnation at lock-acquisition time
};

// ── Factory functions ──────────────────────────────────────────
inline ReadLogEntry_wt make_read_entry(word_t version, word_t incarnation,
                                       const any_type_t &val) {
	return {version, incarnation, val};
}

inline WriteLogEntry_wt make_write_entry(const any_type_t &old_val,
                                         const any_type_t &new_val,
                                         word_t version, word_t incarnation) {
	return {old_val, new_val, version, incarnation};
}

/** -------------------------------------------------------
  * Lock class — inherits from the common Lock.
  * No overrides needed; the base Lock now preserves
  * incarnation in try_lock and has a correct
  * get_incarnation().
  * ---------------------------------------------------- */

class Lock_wt : public Lock
{
};

extern __thread Transaction<ReadLogEntry_wt, WriteLogEntry_wt> *current_tx_wt;
extern LockTable<Lock_wt> g_locks_wt;

/** -------------------------------------------------------
  * Reset all lock versions (WT inherits Lock_wt from common
  * Lock, so reset_versions() works).
  * ---------------------------------------------------- */

static void reset_locks() { g_locks_wt.reset_versions(); }

/** -------------------------------------------------------
  * Random backoff after repeated aborts.
  * ---------------------------------------------------- */

static void      //
random_backoff() //
{
	tinystm::random_backoff(current_tx_wt->abort_count);
}

/** -------------------------------------------------------
  * Thread init/destroy.
  * ---------------------------------------------------- */

inline void   //
init_thread() //
{
	if (!current_tx_wt) {
		current_tx_wt = new Transaction<ReadLogEntry_wt, WriteLogEntry_wt>();
		current_tx_wt->id = thr_counter.fetch_add(1, std::memory_order_acq_rel);
	}
	current_tx_wt->reset();
}

inline void   //
exit_thread() //
{
	if (!current_tx_wt)
		return;
	delete current_tx_wt;
	current_tx_wt = nullptr;
}

/** -------------------------------------------------------
  * begin / abort / commit
  * ---------------------------------------------------- */

inline bool //
begin()     //
{
	auto *tx = current_tx_wt;

	TM_ASSERT(tx, "begin: tx is null");
	if (tx->active)
		return true;

	tx->clear();
	tx->start_version = get_clock();
	tx->end_version = tx->start_version;
	tx->active = true;
	tx->read_only = true;
	if (!tx->is_retry)
		tx->abort_count = 0;
	tx->is_retry = false;

	TM_EVENT(TX_BEGIN, tx->id, tx->start_version);

	return true;
}

inline void                    //
abort_tx(const char *loc = "") //
{
	auto *tx = current_tx_wt;

	TM_ASSERT(tx, "abort_tx: tx is null");
	TM_ASSERT(tx->active, "abort_tx: tx not active");

	TM_EVENT(TX_ABORT, tx->id, tx->abort_count);

	// Restore old values from write-set
	for (auto &it : tx->write_set) {
		write_value_to_addr(it.first, it.second.old_val, ValueType::UINT64);
	}

	// Release locks with incarnation-increment
	for (Lock *l : tx->locks_held) {
		static_cast<Lock_wt *>(l)->inc_abort(0);
	}

	stm::tm_token_release_if_held(tx->id);

	tx->clear();
	tx->active = false;
	tx->abort_count++;
	tx->is_retry = true;
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);

	if (tx->abort_count > 5) {
		random_backoff();
	}

	siglongjmp(*jmpbuf, 1);
	TM_ASSERT(false, "Did not jump");
}

// ── Validate read-set after clock increment ────────────────
// Detects concurrent commits between read and write time.
inline void
validate_read_set_wt(word_t commit_version)
{
	auto *tx = current_tx_wt;
	if (commit_version <= tx->start_version + 1)
		return;
	for (auto &it : tx->read_set) {
		auto &r = it.second;
		ByteOffset bo((word_t)it.first);
		Lock_wt *lock = &g_locks_wt.get(bo.base_addr);

		word_t l = lock->get();
		word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
		bool is_locked = (l & OWNED_MASK) != 0;
		word_t current_version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
		word_t current_incarnation = (l >> OWNED_BITS) & INCARNATION_MASK;

		if (is_locked && owner != tx->id)
			abort_tx("read_lock_contention");
		if (!is_locked && (current_version != r.observed_version ||
		                   current_incarnation != r.observed_incarnation))
			abort_tx("version_mismatch");
		if (is_locked && owner == tx->id) {
			auto w = tx->write_set.find(it.first);
			if (w != tx->write_set.end() &&
			    r.observed_version != w->second.version)
				abort_tx("stale_read_own_lock");
		}
	}
}

// ── Release write-locks with commit version ────────────────
inline void
release_write_locks_wt(word_t commit_version)
{
	auto *tx = current_tx_wt;
	for (auto &it : tx->write_set) {
		ByteOffset bo((word_t)it.first);
		Lock_wt *lock = &g_locks_wt.get(bo.base_addr);
		if (lock->is_locked_by(tx->id)) {
			TM_EVENT2(LOCK_RELEASE, (uint64_t)lock, (uint64_t)it.first, commit_version);
			lock->unlock_with_version(tx->id, commit_version);
		}
	}
}



inline bool //
commit()    //
{
	auto *tx = current_tx_wt;

	TM_ASSERT(tx, "commit: tx is null");
	TM_ASSERT(tx->active, "commit: tx not active");

	if (!tx->read_only && !tx->write_set.empty()) {
		word_t commit_version = increment_clock(tx->id);

		TM_EVENT2(COMMIT_LOCK_ACQUIRE, tx->id, commit_version, tx->write_set.size());

		if (commit_version < tx->end_version)
			abort_tx("commit_version_stale");

		if (commit_version > tx->end_version + 1) {
			TM_EVENT2(GAP_CHECK, tx->id, tx->end_version, commit_version);
		}

		validate_read_set_wt(commit_version);

		TM_EVENT(COMMIT_WRITEBACK, tx->id, tx->write_set.size());
		release_write_locks_wt(commit_version);

		TM_EVENT(COMMIT_SUCCESS, tx->id, commit_version);
	}

	tx->reset();
	stm::tm_token_release();
	return true;
}

static bool                                             //
try_soft_spin(                                          //
    Transaction<ReadLogEntry_wt, WriteLogEntry_wt> *tx, //
    Lock_wt *lock                                       //
)
{
	if (tx->abort_count < TOKEN_SOFT_SPIN_THRESHOLD)
		return false;
	if (!stm::tm_token_is_free())
		return false;
	if (!stm::tm_token_try_acquire(tx->id))
		return false;
	{
		int ts_spin = 0;
		while ((lock->get() & OWNED_MASK) != 0) {
			if (++ts_spin > 5000) return false;
			stm::tm_cpu_relax();
		}
	}
	return true;
}

/** -------------------------------------------------------
  * read_word / write_word
  *
  * Both functions work at the 8-byte-word granularity.
  * The `addr` parameter MUST be the ALIGNED (8-byte) address.
  * Sub-word extraction/merging is the caller's responsibility
  * (see tm_read_i1/tm_write_i1 below).
  * ---------------------------------------------------- */

static any_type_t                                       //
read_word_wt(                                           //
    Transaction<ReadLogEntry_wt, WriteLogEntry_wt> *tx, //
    void *addr,                                         //
    ValueType /*sz*/                                    //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);

	TM_ASSERT(tx, "read_word_wt: tx is null");
	TM_ASSERT(tx->active, "read_word_wt: tx not active");

	// Write-set lookup — return the buffered new value if we wrote here
	{
		auto w = tx->write_set.find(addr);
		if (w != tx->write_set.end()) {
			return w->second.new_val;
		}
	}


	Lock_wt *lock = &g_locks_wt.get(ByteOffset((word_t)addr).base_addr);

	if (lock->is_locked_by(tx->id)) {
		// Self-locked: read directly (write-through => memory has our value)
		return read_value_from_addr(addr, ValueType::UINT64);
	}

	// Double-check protocol
	volatile word_t l = lock->get();
	{
		int spin_count = 0;
		while (true) {
			// Locked by another — re-read until released
			if ((l & OWNED_MASK) != 0) {
				if (++spin_count > 5000) {
					abort_tx("read_spin_timeout");
				}
				l = lock->get();
				continue;
			}

		word_t version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
		word_t incarnation = (l >> OWNED_BITS) & INCARNATION_MASK;
		volatile any_type_t val = read_value_from_addr(addr, ValueType::UINT64);
		volatile word_t l2 = lock->get();

		if (l != l2) {
			l = l2;
			continue;
		}

		if (version > tx->end_version) {
			TM_EVENT2(READ_VERSION_CHECK, (uint64_t)addr, (uint64_t)lock, version);
			// Version-extension (same pattern as WBCTL)
			if (tx->read_only) {
				abort_tx("read_only_version_overflow");
			}

			bool extended = false;
			for (auto &it : tx->read_set) {
				auto &r = it.second;
				Lock_wt *rl = &g_locks_wt.get(ByteOffset((word_t)it.first).base_addr);
				word_t rv = (rl->get() & (VERSION_MASK << META_BITS)) >> META_BITS;
				if (rv > tx->start_version) {
					extended = true;
					break;
				}
			}

			if (!extended) {
				// Try full extend or abort
				word_t last_version = get_clock();
				bool valid = true;
				for (auto &it : tx->read_set) {
					auto &r = it.second;
					Lock_wt *rl = &g_locks_wt.get(ByteOffset((word_t)it.first).base_addr);
					word_t lv = rl->get();
					word_t rv = (lv & (VERSION_MASK << META_BITS)) >> META_BITS;
					if (rv > r.observed_version) {
						valid = false;
						break;
					}
				}
				if (!valid) {
					abort_tx("read_extend_validation");
				}
				tx->end_version = last_version;
				if (tx->end_version < version) {
					abort_tx("read_extend_version_stale");
				}
			} else {
				tx->end_version = get_clock();
				if (tx->end_version < version) {
					abort_tx("read_extend_version_stale_shared");
				}
			}
			continue;
		}

		any_type_t result = {.u8 = val.u8};

		TM_EVENT2(READ_LOCK_ACQUIRE, (uint64_t)addr, (uint64_t)lock, version);

		tx->read_set.insert(std::pair(addr, make_read_entry(version, incarnation, result)));

		return result;
	}
	}
}

static void                                             //
write_word_wt(                                           //
    Transaction<ReadLogEntry_wt, WriteLogEntry_wt> *tx, //
    void *addr,                                         //
    any_type_t val,                                     //
    ValueType /*sz*/                                    //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);

	TM_ASSERT(tx, "write_word_wt: tx is null");
	TM_ASSERT(tx->active, "write_word_wt: tx not active");

	tx->read_only = false;

	// Existing write-set entry at the same aligned address → update in place
	{
		auto w = tx->write_set.find(addr);
		if (w != tx->write_set.end()) {
			w->second.new_val = val;
			write_value_to_addr(addr, val, ValueType::UINT64);
			return;
		}
	}



	Lock_wt *lock = &g_locks_wt.get(ByteOffset((word_t)addr).base_addr);

	{
		int write_spin_count = 0;
		while (true) {
			volatile word_t l = lock->get();

			if ((l & OWNED_MASK) == 0) {
			// Lock is free — try to acquire
			word_t version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
			word_t incarnation = (l >> OWNED_BITS) & INCARNATION_MASK;

			if (lock->try_lock(tx->id)) {
				// Acquired — save old value and write through.
				// Re-read version from the lock AFTER acquisition: the
				// version cached in `l` (from line 377) may be stale if
				// another thread committed and unlocked between line 377
				// and the successful try_lock.  Since try_lock preserves
				// the version bits, reading after CAS gives the true
				// version at lock-acquisition time.
				word_t acquired_state = lock->get();
				version = (acquired_state & (VERSION_MASK << META_BITS)) >> META_BITS;
				incarnation = (acquired_state >> OWNED_BITS) & INCARNATION_MASK;

				any_type_t old_val = read_value_from_addr(addr, ValueType::UINT64);

				WriteLogEntry_wt w = make_write_entry(old_val, val, version, incarnation);
				tx->write_set[addr] = w;
				tx->locks_held.push_back(lock);

				TM_EVENT2(WRITE_LOCK_ACQUIRE, (uint64_t)addr, (uint64_t)lock, l);
				TM_EVENT2(WRITE_SET_INSERT, (uint64_t)addr, (uint64_t)lock, (uint64_t)8);

				write_value_to_addr(addr, val, ValueType::UINT64);
				return;
			} else if (lock->is_locked_by(tx->id)) {
				// CAS failed but the lock is ours — another sub-address in this
				// TX acquired the same lock between our l check and the CAS.
				any_type_t old_val = read_value_from_addr(addr, ValueType::UINT64);

				WriteLogEntry_wt w = make_write_entry(old_val, val, version, incarnation);
				tx->write_set[addr] = w;

				TM_EVENT2(WRITE_SET_INSERT, (uint64_t)addr, (uint64_t)lock, (uint64_t)8);

				write_value_to_addr(addr, val, ValueType::UINT64);
				return;
			} else {
				if (try_soft_spin(tx, lock))
					continue;
				abort_tx("write_lock_contention");
			}
		} else if (lock->is_locked_by(tx->id)) {
			// Lock already held by us
			word_t version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
			word_t incarnation = (l >> OWNED_BITS) & INCARNATION_MASK;
			any_type_t old_val = read_value_from_addr(addr, ValueType::UINT64);

			tx->write_set[addr] = make_write_entry(old_val, val, version, incarnation);
			write_value_to_addr(addr, val, ValueType::UINT64);
			TM_EVENT2(WRITE_SET_INSERT, (uint64_t)addr, (uint64_t)lock, (uint64_t)8);
			return;
		}
		// Lock held by someone else → bounded busy-wait with abort
		if (++write_spin_count > 5000) {
			abort_tx("write_spin_timeout");
		}
		stm::tm_cpu_relax();
	}
	}
}

/** -------------------------------------------------------
  * tm_read / tm_write wrappers.
  *
  * Sub-word types (i1, i2) merge/extract bytes from the
  * full 8-byte word at the aligned address.
  * ---------------------------------------------------- */

inline uint8_t    //
tm_read_i1(       //
    uint8_t *addr //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	void *word_addr = (void *)((word_t)addr & ~(word_t)7);
	any_type_t word = read_word_wt(tx, word_addr, ValueType::UINT8);
	uint8_t off = (word_t)addr & 7;
	return (word.u8 >> (off * 8)) & 0xFF;
}

inline uint16_t    //
tm_read_i2(        //
    uint16_t *addr //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	void *word_addr = (void *)((word_t)addr & ~(word_t)7);
	any_type_t word = read_word_wt(tx, word_addr, ValueType::UINT16);
	uint8_t off = (word_t)addr & 7;
	return (word.u8 >> (off * 8)) & 0xFFFF;
}

inline uint32_t    //
tm_read_i4(        //
    uint32_t *addr //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	any_type_t w = read_word_wt(tx,
	                            (void *)((word_t)addr & ~(word_t)7),
	                            ValueType::UINT32);
	uint8_t off = (word_t)addr & 7;
	return (uint32_t)(w.u8 >> (off * 8));
}

inline uint64_t    //
tm_read_i8(        //
    uint64_t *addr //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	return read_word_wt(tx, (void *)addr, ValueType::UINT64).u8;
}

inline float    //
tm_read_f4(     //
    float *addr //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	any_type_t w = read_word_wt(tx,
	                            (void *)((word_t)addr & ~(word_t)7),
	                            ValueType::FLOAT);
	uint8_t off = (word_t)addr & 7;
	uint32_t bits = (uint32_t)(w.u8 >> (off * 8));
	return *(float *)&bits;
}

inline double    //
tm_read_f8(      //
    double *addr //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	any_type_t w = read_word_wt(tx, (void *)addr, ValueType::DOUBLE);
	return w.f8;
}

inline void *   //
tm_read_ptr(    //
    void **addr //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	any_type_t w = read_word_wt(tx, (void *)addr, ValueType::POINTER);
	return w.ptr;
}

inline void        //
tm_write_i1(       //
    uint8_t *addr, //
    uint8_t val    //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	void *word_addr = (void *)((word_t)addr & ~(word_t)7);
	uint8_t off = (word_t)addr & 7;
	word_t mask = (word_t)0xFF << (off * 8);
	// Read the full 8-byte word through the TM read path so that a
	// read-set entry is added with the observed lock version.  This
	// enables the own-lock validation at commit to detect concurrent
	// commits between this RMW read and the lock acquisition inside
	// write_word_wt.
	any_type_t full = read_word_wt(tx, word_addr, ValueType::UINT64);
	word_t word = full.u8;
	word = (word & ~mask) | ((word_t)val << (off * 8));
	any_type_t w = {.u8 = word};
	write_word_wt(tx, word_addr, w, ValueType::UINT8);
}

inline void         //
tm_write_i2(        //
    uint16_t *addr, //
    uint16_t val    //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	void *word_addr = (void *)((word_t)addr & ~(word_t)7);
	uint8_t off = (word_t)addr & 7;
	word_t mask = (word_t)0xFFFF << (off * 8);
	any_type_t full = read_word_wt(tx, word_addr, ValueType::UINT64);
	word_t word = full.u8;
	word = (word & ~mask) | ((word_t)val << (off * 8));
	any_type_t w = {.u8 = word};
	write_word_wt(tx, word_addr, w, ValueType::UINT16);
}

inline void         //
tm_write_i4(        //
    uint32_t *addr, //
    uint32_t val    //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	void *word_addr = (void *)((word_t)addr & ~(word_t)7);
	uint8_t off = (word_t)addr & 7;
	word_t mask = (word_t)0xFFFFFFFF << (off * 8);
	any_type_t full = read_word_wt(tx, word_addr, ValueType::UINT64);
	word_t word = full.u8;
	word = (word & ~mask) | ((word_t)val << (off * 8));
	any_type_t w = {.u8 = word};
	write_word_wt(tx, word_addr, w, ValueType::UINT32);
}

inline void         //
tm_write_i8(        //
    uint64_t *addr, //
    uint64_t val    //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	any_type_t w = {.u8 = val};
	write_word_wt(tx, (void *)addr, w, ValueType::UINT64);
}

inline void      //
tm_write_f4(     //
    float *addr, //
    float val    //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	void *word_addr = (void *)((word_t)addr & ~(word_t)7);
	uint8_t off = (word_t)addr & 7;
	word_t mask = (word_t)0xFFFFFFFF << (off * 8);
	any_type_t full = read_word_wt(tx, word_addr, ValueType::UINT64);
	word_t word = full.u8;
	word_t fbits;
	memcpy(&fbits, &val, sizeof(float));
	word = (word & ~mask) | (fbits << (off * 8));
	any_type_t w = {.u8 = word};
	write_word_wt(tx, word_addr, w, ValueType::FLOAT);
}

inline void       //
tm_write_f8(      //
    double *addr, //
    double val    //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	any_type_t w = {.f8 = val};
	write_word_wt(tx, (void *)addr, w, ValueType::DOUBLE);
}

inline void      //
tm_write_ptr(    //
    void **addr, //
    void *val    //
)
{
	auto *tx = current_tx_wt;
	TM_ASSERT_VALID_TX(tx, "tinystm wt");

	any_type_t w = {.ptr = val};
	write_word_wt(tx, (void *)addr, w, ValueType::POINTER);
}

} // namespace tinystm
