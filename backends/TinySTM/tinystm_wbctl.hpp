/**
 * TinySTM - WRITE_BACK_CTL (Commit-Time Locking)
 *
 * ══════════════════════════════════════════════════════════════════
 *  This code runs in the RUNTIME, which is compiled SEPARATELY from
 *  user code and is NEVER fed through the TM plugin.  Every function
 *  and data structure here uses the STANDARD C++ allocator.
 *  ⚠ Do NOT add operator new/delete overrides that route to tm_malloc
 *    — see tm_alloc_overrides.hpp for the full explanation.
 * ══════════════════════════════════════════════════════════════════
 */

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <thread>

#include "tinystm_common.hpp"
#include "../tm_log_entries.hpp"
#include "../tm_log_merge.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
}

namespace tinystm
{

constexpr const char *VERSION = "0.2.0-wbctl";

struct ReadLogEntry_wbctl : stm::BitmapReadEntry<void *> {};

struct WriteLogEntry_wbctl : stm::BitmapRedoEntry<void *> {
	word_t version;
};

class Lock_wbctl : public Lock
{
public:
	bool is_locked() const
	{
		return (state.load(std::memory_order_acquire) & OWNED_MASK) != 0;
	}
};

extern __thread Transaction<ReadLogEntry_wbctl, WriteLogEntry_wbctl> *current_tx_wbctl;
extern LockTable<Lock_wbctl> g_locks_wbctl;

static void reset_locks() { g_locks_wbctl.reset_versions(); }

/** -------------------------------------------------------
  * Stubs for initialization/destruction.
  * ---------------------------------------------------- */

inline void   //
init_thread() //
{
	if (!current_tx_wbctl) {
		current_tx_wbctl = new Transaction<ReadLogEntry_wbctl, WriteLogEntry_wbctl>();
		current_tx_wbctl->id = thr_counter.fetch_add(1, std::memory_order_acq_rel);
	}
	current_tx_wbctl->reset();
}

inline void   //
exit_thread() //
{
	if (!current_tx_wbctl)
		return;
	delete current_tx_wbctl;
	current_tx_wbctl = nullptr;
}

static void      //
random_backoff() //
{
	tinystm::random_backoff(current_tx_wbctl->abort_count);
}

/** -------------------------------------------------------
  * Stubs for Transaction begin/end.
  * ---------------------------------------------------- */

inline bool //
begin()     //
{
	auto *tx = current_tx_wbctl;

	TM_ASSERT(tx, "tx not defined");
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

	return true;
}

inline void                    //
abort_tx(const char *loc = "") //
{
	auto *tx = current_tx_wbctl;

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
	auto *tx = current_tx_wbctl;
	for (auto &it : tx->read_set) {
		auto &addr = it.first;
		auto &r = it.second;
		ByteOffset bo((word_t)addr);
		Lock *lock = &g_locks_wbctl.get(bo.base_addr);
		word_t l = lock->get();
		word_t is_locked = (l & OWNED_MASK) != 0;
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
	auto *tx = current_tx_wbctl;
	volatile word_t last_version = get_clock();
	if (!validate())
		return false;
	tx->end_version = last_version; // extend the version
	return true;
}

static bool        //
compareByAddr(     //
    const void *a, //
    const void *b  //
)
{
	return (word_t)a < (word_t)b;
}

// ── Lock acquisition phase ──────────────────────────────────
// Collect write-set addresses, sort for global lock ordering,
// and acquire each lock with soft-spin on contention.
inline void
lock_write_set()
{
	auto *tx = current_tx_wbctl;
	std::vector<void *> sorted_addrs;
	sorted_addrs.reserve(tx->write_set.size());
	for (auto &it : tx->write_set)
		sorted_addrs.push_back(it.first);
	if (tx->abort_count > 3)
		std::sort(sorted_addrs.begin(), sorted_addrs.end(), compareByAddr);
	for (void *addr : sorted_addrs) {
		auto &w = tx->write_set[addr];
		ByteOffset bo((word_t)addr);
		Lock *lock = &g_locks_wbctl.get(bo.base_addr);
		word_t l = lock->get();
		word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
		if (owner != tx->id) {
			while (!lock->try_lock(tx->id)) {
				if (!extend()) {
					if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 5)) {
						continue;
					}
					abort_tx("commit_lock");
				}
			}
			tx->locks_held.push_back(lock);
		}
		TM_ASSERT(lock->is_locked() && lock->get_owner() == tx->id,
		          "Lock not locked or wrong owner");
	}
}

// ── Clock increment + gap check + read-set validation ──────
inline word_t
increment_clock_and_validate()
{
	auto *tx = current_tx_wbctl;
	word_t commit_version = increment_clock(tx->id);
	if (commit_version < tx->end_version)
		abort_tx("version_overflow");
	if (commit_version != tx->end_version + 1) {
		if (!extend()) {
			abort_tx("gap_check");
		}
	}
	if (!validate()) {
		abort_tx("read_validation");
	}
	return commit_version;
}

// ── Write-back phase ───────────────────────────────────────
inline void
write_back_bitmap()
{
	auto *tx = current_tx_wbctl;
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
}

// ── Release write-locks with commit version ────────────────
inline void
release_write_locks(word_t commit_version)
{
	auto *tx = current_tx_wbctl;
	for (auto &it : tx->write_set) {
		auto &addr = it.first;
		auto &w = it.second;
		ByteOffset bo((word_t)addr);
		Lock *lock = &g_locks_wbctl.get(bo.base_addr);
		if (!lock->is_locked_by(tx->id)) {
			continue;
		}
		if (lock->get_version() > commit_version) {
			lock->unlock(tx->id);
		} else {
			TM_ASSERT(lock->get_version() <= commit_version,
			          "Lock version updated while locked");
			lock->unlock_with_version(tx->id, commit_version);
		}
		TM_ASSERT(lock->get_version() >= commit_version, "Lock version not updated");
	}
	tx->locks_held.clear();
}

inline bool //
commit()    //
{
	auto *tx = current_tx_wbctl;

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	if (!tx->read_only) {
		lock_write_set();
		word_t commit_version = increment_clock_and_validate();
		write_back_bitmap();
		release_write_locks(commit_version);
	}

	stm::tm_token_release();
	tx->reset();
	return true;
}

/** -------------------------------------------------------
  * Stubs for Transaction read/write instrumentation.
  * ---------------------------------------------------- */

inline any_type_t                                             //
read_word_ctl(                                                //
    Transaction<ReadLogEntry_wbctl, WriteLogEntry_wbctl> *tx, //
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

	Lock *lock = &g_locks_wbctl.get(bo.base_addr);
	TM_ASSERT(!lock->is_locked_by(tx->id), "wbctl locks at commit time");
	volatile word_t l = lock->get();

	while (true) {
		if ((l & OWNED_MASK) != 0) {
			l = lock->get();
			continue;
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
				if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 5)) {
					continue;
				}
				abort_tx("read_version_check");
			}
		}

		any_type_t val = {.u8 = value.u8};

		auto r_it = tx->read_set.find(aligned);
		if (r_it != tx->read_set.end()) {
			stm::merge::merge_read(r_it->second, version, val, sz, addr);
		} else {
			tx->read_set.insert(std::pair(aligned, stm::merge::make_read_entry<ReadLogEntry_wbctl>(aligned, version, val, sz, addr)));
		}

		return val;
	}
}

inline void                                                   //
write_word_ctl(                                                //
    Transaction<ReadLogEntry_wbctl, WriteLogEntry_wbctl> *tx, //
    void *addr,                                               //
    any_type_t val,                                           //
    ValueType sz                                              //
)
{
	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	tx->read_only = false;

	void *aligned = stm::merge::align_down_8(addr);
	ByteOffset bo((word_t)aligned);

	// Found write-set entry at aligned address → bitmap merge
	{
		auto w = tx->write_set.find(aligned);
		if (w != tx->write_set.end()) {
			stm::merge::bitmap_write(w->second.value, w->second.valid, val, sz, addr);
			auto r_it = tx->read_set.find(aligned);
			if (r_it != tx->read_set.end()) {
				unsigned nbytes = stm::type_size(sz);
				unsigned shift = stm::merge::byte_offset(addr) * 8;
				uint8_t need = static_cast<uint8_t>(BYTE_MASK(nbytes) << shift);
				r_it->second.valid |= need;
				uint64_t clr = static_cast<uint64_t>(BYTE_MASK(nbytes)) << shift;
				r_it->second.value = (r_it->second.value & ~clr) |
				                     (any_to_u64(val, sz) << shift);
			}
			return;
		}
	}

	while (true) {
		Lock *lock = &g_locks_wbctl.get(bo.base_addr);
		volatile word_t l = lock->get();
		word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
		bool is_locked = (l & OWNED_MASK) != 0;

		TM_ASSERT(owner != tx->id, "WBCTL only locks at commit time");

		if (is_locked && !validate()) {
			if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 5)) {
				continue;
			}
			abort_tx("write_lock_spin_validate");
		}

		if (is_locked)
			continue;

		volatile word_t l2 = lock->get();
		if (l != l2) {
			l = l2;
			continue;
		}

		word_t version = (l2 & (VERSION_MASK << META_BITS)) >> META_BITS;

		if (version > tx->end_version) {
			if (extend()) {
				continue;
			} else {
				abort_tx("write_version_extension");
			}
		}

		tx->write_set[aligned] = stm::merge::make_write_entry<WriteLogEntry_wbctl>(aligned, val, sz, addr, version);
		tx->read_set.insert(std::pair(aligned, stm::merge::make_read_entry<ReadLogEntry_wbctl>(aligned, version, val, sz, addr)));

		return;
	}
}

/** -------------------------------------------------------
  * Stubs for Transaction read/write instrumentation.
  * ---------------------------------------------------- */

#define TM_STUB_TX current_tx_wbctl
#define TM_STUB_READ_FN read_word_ctl
#define TM_STUB_WRITE_FN write_word_ctl
#define TM_STUB_HAVE_TYPES
#define TM_STUB_RL ReadLogEntry_wbctl
#define TM_STUB_WL WriteLogEntry_wbctl
#include "../tm_stubs.hpp"

} // namespace tinystm
