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

#include "tinystm_common.hpp"
#include "../tm_spin_token.hpp"

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

static void    //
init_rand()    //
{
	if (!rng_initialized) {
		std::random_device rd;
		rng.seed(rd());
		rng_initialized = true;
	}
}

static void      //
random_backoff() //
{
	if (!rng_initialized) {
		init_rand();
	}
	std::exponential_distribution<> dist(
	    (double)1 / (double)(current_tx_wt->abort_count + 1));
	int delay = std::min(dist(rng), 1e5);
	std::this_thread::sleep_for(std::chrono::microseconds(delay));
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
	if (!tx->is_retry) tx->abort_count = 0;
	tx->is_retry = false;

	return true;
}

inline void //
abort_tx()  //
{
	auto *tx = current_tx_wt;

	TM_ASSERT(tx, "abort_tx: tx is null");
	TM_ASSERT(tx->active, "abort_tx: tx not active");

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

inline bool //
commit()    //
{
	auto *tx = current_tx_wt;
	word_t commit_version = 0;

	TM_ASSERT(tx, "commit: tx is null");
	TM_ASSERT(tx->active, "commit: tx not active");

	if (!tx->read_only && !tx->write_set.empty()) {

		commit_version = increment_clock(tx->id);
		if (commit_version < tx->end_version) {
			abort_tx();
		}

		// Validate read-set — detect concurrent commits between read and write
		if (commit_version > tx->start_version + 1) {
			for (auto &it : tx->read_set) {
				auto &r = it.second;
				ByteOffset bo((word_t)it.first);
				Lock_wt *lock = &g_locks_wt.get(bo.base_addr);

				word_t l = lock->get();
				word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
				bool is_locked = (l & OWNED_MASK) != 0;
				word_t current_version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
				word_t current_incarnation = (l >> OWNED_BITS) & INCARNATION_MASK;

				if (is_locked && owner != tx->id) {
					abort_tx();
				}
				if (!is_locked &&
				    (current_version != r.observed_version ||
				     current_incarnation != r.observed_incarnation)) {
					abort_tx();
				}
				if (is_locked && owner == tx->id) {
					// Our own write-lock — check for stale read between
					// read_time and lock_acquisition_time.
					auto w = tx->write_set.find(it.first);
					if (w != tx->write_set.end() &&
					    r.observed_version != w->second.version) {
						abort_tx();
					}
				}
			}
		}

		// Release write-locks with the commit version
		for (auto &it : tx->write_set) {
			ByteOffset bo((word_t)it.first);
			Lock_wt *lock = &g_locks_wt.get(bo.base_addr);
			if (lock->is_locked_by(tx->id)) {
				lock->unlock_with_version(tx->id, commit_version);
			}
		}
	}

	tx->reset();
	stm::tm_token_release();
	return true;
}

static bool                                                     //
try_soft_spin(                                                  //
    Transaction<ReadLogEntry_wt, WriteLogEntry_wt> *tx,         //
    Lock_wt *lock                                               //
)
{
	if (tx->abort_count < TOKEN_SOFT_SPIN_THRESHOLD)
		return false;
	if (!stm::tm_token_is_free())
		return false;
	if (!stm::tm_token_try_acquire(tx->id))
		return false;
	while ((lock->get() & OWNED_MASK) != 0) {
		TINY_STM_PAUSE();
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

static any_type_t                                              //
read_word_wt(                                                  //
    Transaction<ReadLogEntry_wt, WriteLogEntry_wt> *tx,        //
    void *addr,                                                //
    ValueType /*sz*/                                           //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);

	TM_ASSERT(tx, "read_word_wt: tx is null");
	TM_ASSERT(tx->active, "read_word_wt: tx not active");

	// Stack-address detection: reading from the stack would create read-set
	// entries for stack addresses that hash to random locks, causing spurious
	// validation failures and aborts.
	if (isStackAddress(addr))
		return read_value_from_addr(addr, ValueType::UINT64);

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
	while (true) {
		// Locked by another — soft-spin if abort_count >= threshold
		if ((l & OWNED_MASK) != 0) {
			if (try_soft_spin(tx, lock)) {
				l = lock->get();
				continue;
			}
			abort_tx();
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
			// Version-extension (same pattern as WBCTL)
			if (tx->read_only) {
				abort_tx();
			}

			bool extended = false;
			for (auto &it : tx->read_set) {
				auto &r = it.second;
				Lock_wt *rl = &g_locks_wt.get(
				    ByteOffset((word_t)it.first).base_addr);
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
					Lock_wt *rl = &g_locks_wt.get(
					    ByteOffset((word_t)it.first).base_addr);
					word_t lv = rl->get();
					word_t rv = (lv & (VERSION_MASK << META_BITS)) >> META_BITS;
					if (rv > r.observed_version) {
						valid = false;
						break;
					}
				}
				if (!valid) {
					abort_tx();
				}
				tx->end_version = last_version;
				if (tx->end_version < version) {
					abort_tx();
				}
			} else {
				tx->end_version = get_clock();
				if (tx->end_version < version) {
					abort_tx();
				}
			}
		}

		any_type_t result = {.u8 = val.u8};

		ReadLogEntry_wt r;
		r.observed_version = version;
		r.observed_incarnation = incarnation;
		r.observed_val = result;
		tx->read_set.insert(std::pair(addr, r));

		return result;
	}
}

static void                                                    //
write_word_wt(                                                 //
    Transaction<ReadLogEntry_wt, WriteLogEntry_wt> *tx,        //
    void *addr,                                                //
    any_type_t val,                                            //
    ValueType /*sz*/                                           //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);

	TM_ASSERT(tx, "write_word_wt: tx is null");
	TM_ASSERT(tx->active, "write_word_wt: tx not active");

	// Stack-address detection: writing to the stack via tm_write would create
	// a write-set entry that gets written back at commit time — by then the
	// stack frame has been popped and the write corrupts active stack data.
	if (isStackAddress(addr)) {
		write_value_to_addr(addr, val, ValueType::UINT64);
		return;
	}

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

				WriteLogEntry_wt w;
				w.old_val = old_val;
				w.new_val = val;
				w.version = version;
				w.incarnation = incarnation;
			tx->write_set[addr] = w;
			tx->locks_held.push_back(lock);

				write_value_to_addr(addr, val, ValueType::UINT64);
				return;
			} else if (lock->is_locked_by(tx->id)) {
				// CAS failed but the lock is ours — another sub-address in this
				// TX acquired the same lock between our l check and the CAS.
				any_type_t old_val = read_value_from_addr(addr, ValueType::UINT64);

			WriteLogEntry_wt w;
			w.old_val = old_val;
			w.new_val = val;
			w.version = version;
			w.incarnation = incarnation;
			tx->write_set[addr] = w;

			write_value_to_addr(addr, val, ValueType::UINT64);
			return;
		} else {
			if (try_soft_spin(tx, lock))
				continue;
			abort_tx();
		}
	} else if (lock->is_locked_by(tx->id)) {
		// Lock already held by us
			word_t version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
			word_t incarnation = (l >> OWNED_BITS) & INCARNATION_MASK;
			any_type_t old_val = read_value_from_addr(addr, ValueType::UINT64);

			WriteLogEntry_wt w;
			w.old_val = old_val;
			w.new_val = val;
			w.version = version;
			write_value_to_addr(addr, val, ValueType::UINT64);
			return;
		} else {
			if (try_soft_spin(tx, lock))
				continue;
			abort_tx();
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

	any_type_t w = read_word_wt(tx, (void *)((word_t)addr & ~(word_t)7), ValueType::UINT32);
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

	any_type_t w = read_word_wt(tx, (void *)((word_t)addr & ~(word_t)7), ValueType::FLOAT);
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
