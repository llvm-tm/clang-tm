/**
 * TinySTM - write-back encounter-time locking
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

#include "tinystm_common.hpp"
#include "../tm_log_entries.hpp"
#include "../tm_log_merge.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
}

namespace tinystm
{

constexpr const char *VERSION = "0.2.0-wbetl";

struct ReadLogEntry_wbetl : stm::BitmapReadEntry<void *> {};

struct WriteLogEntry_wbetl : stm::BitmapRedoEntry<void *> {
	volatile word_t version;
};

class Lock_wbetl : public Lock
{
public:
	void unlock() { state.store(0, std::memory_order_release); }

	void set_version(word_t v)
	{
		word_t old = state.load(std::memory_order_acquire);
		while (true) {
			word_t locked = old & OWNED_MASK;
			word_t new_val = (v & VERSION_MASK) | locked;
			if (state.compare_exchange_weak(old,
			                                new_val,
			                                std::memory_order_release,
			                                std::memory_order_acquire)) {
				break;
			}
		}
	}

	bool is_locked() const
	{
		return (state.load(std::memory_order_acquire) & OWNED_MASK) != 0;
	}

	word_t get_owner() const
	{
		return (state.load(std::memory_order_acquire) & (THREAD_MASK << LOCK_BITS)) >>
		       LOCK_BITS;
	}
};

extern __thread Transaction<ReadLogEntry_wbetl, WriteLogEntry_wbetl> *current_tx_wbetl;
extern LockTable<Lock_wbetl> g_locks_wbetl;

static void reset_locks() { g_locks_wbetl.reset_versions(); }

/** -------------------------------------------------------
  * Stubs for initialization/destruction.
  * ---------------------------------------------------- */

inline void   //
init_thread() //
{
	if (!current_tx_wbetl) {
		current_tx_wbetl = new Transaction<ReadLogEntry_wbetl, WriteLogEntry_wbetl>();
		current_tx_wbetl->id = thr_counter.fetch_add(1, std::memory_order_acq_rel);
	}
	current_tx_wbetl->reset();
}

inline void   //
exit_thread() //
{
	if (!current_tx_wbetl)
		return;
	delete current_tx_wbetl;
	current_tx_wbetl = nullptr;
}

/** -------------------------------------------------------
  * Stubs for Transaction begin/end.
  * ---------------------------------------------------- */

static void      //
random_backoff() //
{
	tinystm::random_backoff(current_tx_wbetl->abort_count);
}

inline bool //
begin()     //
{
	auto *tx = current_tx_wbetl;

	TM_ASSERT(tx, "tx not defined");
	if (tx->active)
		return true;

	TM_ASSERT(!tx->aborted, "begin: stale aborted flag");

	tx->start_version = get_clock();
	tx->end_version = tx->start_version;
	tx->active = true;
	tx->read_only = true;
	if (!tx->is_retry)
		tx->abort_count = 0;
	tx->is_retry = false;

	return true;
}

inline void                    //
abort_tx(const char *loc = "") //
{
	auto *tx = current_tx_wbetl;

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	tx->unlock_held_locks_and_clear();
	tx->abort_count++;
	tx->is_retry = true;
	stm::tm_token_release_if_held(tx->id);
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);

	if (tx->abort_count > 5) {
		random_backoff();
	}
	siglongjmp(*jmpbuf, 1);
	TM_ASSERT(false, "Did not jump");
}

inline bool //
validate()  //
{
	auto *tx = current_tx_wbetl;
	for (auto &it : tx->read_set) {
		auto &addr = it.first;
		auto &r = it.second;
		ByteOffset bo((word_t)addr);
		Lock *lock = &g_locks_wbetl.get(bo.base_addr);
		word_t l = lock->get();
		bool is_locked = (l & OWNED_MASK) != 0;
		word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
		word_t current_version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;

		if ((is_locked && owner != tx->id) || current_version > r.observed_version) {
			return false;
		}
	}
	return true;
}

inline bool //
extend()    //
{
	auto *tx = current_tx_wbetl;
	volatile word_t last_version = get_clock();
	if (!validate())
		return false;
	tx->end_version = last_version; // extend the version
	return true;
}

inline bool //
commit()    //
{
	auto *tx = current_tx_wbetl;

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	TM_ASSERT(!tx->aborted, "commit: stale aborted flag");

	// Locks already taken: write-back

	if (!tx->read_only) {
		word_t commit_version = increment_clock(tx->id);

		if (!extend())
			abort_tx("extend_failed");

		for (auto &it : tx->write_set) {
			auto *aligned = static_cast<void *>(it.first);
			auto &w = it.second;
			for (unsigned byte_off = 0; byte_off < 8; byte_off++) {
				if (w.valid & (1 << byte_off)) {
					void *byte_addr =
					    reinterpret_cast<void *>((uintptr_t)aligned + byte_off);
					uint8_t byte_val =
					    static_cast<uint8_t>((w.value >> (byte_off * 8)) & 0xFF);
					write_value_to_addr(byte_addr,
					                    any_type_t{.u1 = byte_val},
					                    ValueType::UINT8);
				}
			}
		}
		for (Lock *lock : tx->locks_held) {
			lock->unlock_with_version(tx->id, commit_version);
		}
	}

	stm::tm_token_release();
	tx->reset();
	return true;
}

/** -------------------------------------------------------
  * Stubs for Transaction read/write instrumentation.
  * ---------------------------------------------------- */

inline any_type_t                                             //
read_word_etl(                                                //
    Transaction<ReadLogEntry_wbetl, WriteLogEntry_wbetl> *tx, //
    void *addr,                                               //
    ValueType sz                                              //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);
	void *aligned = stm::merge::align_down_8(addr);
	ByteOffset bo((word_t)aligned);

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	// Check write-set by aligned address → bitmap extract
	{
		auto w = tx->write_set.find(aligned);
		if (w != tx->write_set.end()) {
			any_type_t result;
			if (stm::merge::bitmap_read(result, w->second.value, w->second.valid,
			                           sz, addr)) {
				return result;
			}
		}
	}

	TM_ASSERT(!tx->aborted, "read_word_etl: stale aborted flag");

	Lock *lock = &g_locks_wbetl.get(bo.base_addr);
	if (lock->is_locked_by(tx->id)) {
		return read_value_from_addr(addr, sz);
	}

	volatile word_t l = lock->get();

	while (true) {
		if ((l & OWNED_MASK) != 0) {
			if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 3)) {
				while ((lock->get() & OWNED_MASK) != 0) {
					TINY_STM_PAUSE();
				}
				l = lock->get();
				continue;
			}
			abort_tx("read_lock_spin");
		}

		word_t version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
		volatile any_type_t value = read_value_from_addr(addr, sz);
		volatile word_t l2 = lock->get();

		if (l != l2) {
			l = l2;
			continue;
		}

		if (version > tx->end_version) {
			if (extend()) {
				continue;
			} else {
				abort_tx("read_version_extend");
			}
		}

		any_type_t val = {.u8 = value.u8};

		auto r_it = tx->read_set.find(aligned);
		if (r_it != tx->read_set.end()) {
			stm::merge::merge_read(r_it->second, version, val, sz, addr);
		} else {
			tx->read_set.insert(std::pair(aligned, stm::merge::make_read_entry<ReadLogEntry_wbetl>(aligned, version, val, sz, addr)));
		}

		return val;
	}
}

inline void                                                   //
write_word_etl(                                                //
    Transaction<ReadLogEntry_wbetl, WriteLogEntry_wbetl> *tx, //
    void *addr,                                               //
    any_type_t val,                                           //
    ValueType sz                                              //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);
	void *aligned = stm::merge::align_down_8(addr);
	ByteOffset bo((word_t)aligned);

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT_VALID_TX(tx, "write_word_etl");

	TM_ASSERT(!tx->aborted, "write_word_etl: stale aborted flag");

	tx->read_only = false;

	{
		auto w = tx->write_set.find(aligned);
		if (w != tx->write_set.end()) {
			stm::merge::bitmap_write(w->second.value, w->second.valid, val, sz, addr);
			return;
		}
	}

	Lock *lock = &g_locks_wbetl.get(bo.base_addr);

	if (lock->is_locked()) {
		if (lock->get_owner() != tx->id) {
			if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 3)) {
				while (lock->is_locked() && lock->get_owner() != tx->id) {
					TINY_STM_PAUSE();
				}
				if (lock->get_owner() != tx->id) {
					if (!lock->try_lock(tx->id))
						abort_tx("write_try_lock_after_spin");
				}
			} else {
				abort_tx("write_lock_contention");
			}
		}
	} else {
		if (!lock->try_lock(tx->id)) {
			if (!validate())
				abort_tx("write_validate_fail");
			if (!lock->try_lock(tx->id)) {
				if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 3)) {
					while (lock->is_locked()) {
						TINY_STM_PAUSE();
					}
					if (!lock->try_lock(tx->id))
						abort_tx("write_unlock_race");
				} else {
					abort_tx("write_lock_spin_exhausted");
				}
			}
		}
	}

	{
		bool already_held = false;
		for (Lock *hl : tx->locks_held) {
			if (hl == lock) {
				already_held = true;
				break;
			}
		}
		if (!already_held) {
			tx->locks_held.push_back(lock);
		}
	}

	tx->write_set[aligned] = stm::merge::make_write_entry<WriteLogEntry_wbetl>(aligned, val, sz, addr, lock->get_version());
}

#define TM_STUB_TX current_tx_wbetl
#define TM_STUB_READ_FN read_word_etl
#define TM_STUB_WRITE_FN write_word_etl
#define TM_STUB_HAVE_TYPES
#define TM_STUB_RL ReadLogEntry_wbetl
#define TM_STUB_WL WriteLogEntry_wbetl
#include "../tm_stubs.hpp"

} // namespace tinystm
