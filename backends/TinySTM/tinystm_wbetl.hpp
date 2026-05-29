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

extern "C" {
extern __thread int32_t tm_nested_call_counter;
}

namespace tinystm
{

constexpr const char *VERSION = "0.2.0-wbetl";

struct ReadLogEntry_wbetl {
	volatile word_t observed_version;
	any_type_t observed_val;
	ValueType type;
};

struct WriteLogEntry_wbetl {
	any_type_t old_val;
	any_type_t new_val;
	ValueType type;
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
		return (state.load(std::memory_order_acquire) &
		        (THREAD_MASK << LOCK_BITS)) >>
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

static void  //
init_rand()  //
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
	    (double)1 / (double)(current_tx_wbetl->abort_count + 1));
	int delay = std::min(dist(rng), 100000.0);
	std::this_thread::sleep_for(std::chrono::microseconds(delay));
}

inline bool //
begin()     //
{
	auto *tx = current_tx_wbetl;

	TINYSTM_ASSERT(tx, "tx not defined");
	if (tx->active)
		return true;

	if (tx->aborted) {
		tx->unlock_held_locks_and_clear();
		tx->aborted = false;
	}

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
	auto *tx = current_tx_wbetl;

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	tx->unlock_held_locks_and_clear();
	tx->abort_count++;
	tx->is_retry = true;
	stm::tm_token_release_if_held(tx->id);
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);

	if (tx->abort_count > 5) {
		random_backoff();
	}
	siglongjmp(*jmpbuf, 1);
	TINYSTM_ASSERT(false, "Did not jump");
}

static void       //
deferred_abort() //
{
	auto *tx = current_tx_wbetl;
	TINYSTM_ASSERT(tx, "tx not defined");
	tx->unlock_held_locks_and_clear();
	tx->aborted = false;
	tx->abort_count++;
	tx->is_retry = true;
	stm::tm_token_release_if_held(tx->id);
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
	if (tx->abort_count > 5) {
		random_backoff();
	}
	siglongjmp(*jmpbuf, 1);
	TINYSTM_ASSERT(false, "Did not jump");
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

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	if (tx->aborted) {
		deferred_abort();
	}

	// Locks already taken: write-back

	if (!tx->read_only) {
		word_t commit_version = increment_clock(tx->id);

		if (!extend())
			abort_tx();

		for (auto &it : tx->write_set) {
			auto &addr = it.first;
			auto &w = it.second;
			write_value_to_addr(addr, w.new_val, w.type);
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

inline any_type_t                                              //
read_word_etl(                                                //
    Transaction<ReadLogEntry_wbetl, WriteLogEntry_wbetl> *tx, //
    void *addr,                                               //
    ValueType sz                                              //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);
	ByteOffset bo((word_t)addr);

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	// Stack-address detection: reading from the stack would create read-set
	// entries for stack addresses that hash to random locks, causing spurious
	// validation failures and aborts.
	if (isStackAddress(addr))
		return read_value_from_addr(addr, sz);

	// Check write-set for this exact address
	auto w = tx->write_set.find(addr);
	if (w != tx->write_set.end()) {
		if (w->second.type == sz)
			return w->second.new_val;
		// Type mismatch: wider write covers this address, extract bytes.
		if (w->second.type == ValueType::UINT64 && sz == ValueType::UINT8) {
			any_type_t result;
			result.u1 = static_cast<uint8_t>(w->second.new_val.u8 & 0xFF);
			return result;
		}
	}

	// For unaligned reads: check the 8-byte-aligned address.
	if (bo.offset != 0) {
		void *base_addr = reinterpret_cast<void *>(bo.base_addr);
		auto w2 = tx->write_set.find(base_addr);
		if (w2 != tx->write_set.end() && w2->second.type == ValueType::UINT64) {
			unsigned shift = static_cast<unsigned>(bo.offset) * 8;
			switch (sz) {
			case ValueType::UINT8: {
				any_type_t result;
				result.u1 = static_cast<uint8_t>(w2->second.new_val.u8 >> shift);
				return result;
			}
			case ValueType::UINT16: {
				any_type_t result;
				result.u2 = static_cast<uint16_t>(w2->second.new_val.u8 >> shift);
				return result;
			}
			case ValueType::UINT32: {
				any_type_t result;
				result.u4 = static_cast<uint32_t>(w2->second.new_val.u8 >> shift);
				return result;
			}
			default:
				break;
			}
		}
	}

	if (tx->aborted) {
		return read_value_from_addr(addr, sz);
	}

	Lock *lock = &g_locks_wbetl.get(bo.base_addr);
	if (lock->is_locked_by(tx->id)) {
		return read_value_from_addr(addr, sz);
	}

	volatile word_t l = lock->get();

	while (true) {
		if ((l & OWNED_MASK) != 0) {
			// ── Soft-spin on held lock (token as tie-breaker) ──
			if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 3)) {
				while ((lock->get() & OWNED_MASK) != 0) {
					TINY_STM_PAUSE();
				}
				l = lock->get();
				continue;
			}
			tx->aborted = true;
			return read_value_from_addr(addr, sz);
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
				continue; // needs to read again
			} else {
				abort_tx(); // returns to begin
			}
		}

		any_type_t val = {.u8 = value.u8};

		ReadLogEntry_wbetl r;
		r.observed_version = version;
		r.observed_val = val;
		r.type = sz;
		tx->read_set.insert(std::pair(addr, r));

		return val;
	}
}

inline void                                                   //
write_word_etl(                                               //
    Transaction<ReadLogEntry_wbetl, WriteLogEntry_wbetl> *tx, //
    void *addr,                                               //
    any_type_t val,                                           //
    ValueType sz                                              //
)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);
	ByteOffset bo((word_t)addr);

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	// Stack-address detection: writing to the stack via tm_write would create
	// a write-set entry that gets written back at commit time — by then the
	// stack frame has been popped and the write corrupts active stack data.
	if (isStackAddress(addr)) {
		write_value_to_addr(addr, val, sz);
		return;
	}

	if (tx->aborted)
		return;

	tx->read_only = false;

	// Helper: byte width of a ValueType
	auto typeSize = [](ValueType t) -> unsigned {
		switch (t) {
		case ValueType::UINT8:   return 1;
		case ValueType::UINT16:  return 2;
		case ValueType::UINT32:  return 4;
		case ValueType::FLOAT:   return 4;
		case ValueType::UINT64:  return 8;
		case ValueType::POINTER: return 8;
		case ValueType::DOUBLE:  return 8;
		default:                 return 0;
		}
	};

	unsigned sz_bytes = typeSize(sz);

	// Found write-set entry at exact addr with matching type → update in place.
	{
		auto w = tx->write_set.find(addr);
		if (w != tx->write_set.end()) {
			if (w->second.type == sz) {
				w->second.new_val = val;
				return;
			}
			if (w->second.type == ValueType::UINT64 && sz == ValueType::UINT8) {
				uint64_t merged = (w->second.new_val.u8 & ~(uint64_t)0xFF);
				merged |= (uint64_t)(val.u1);
				w->second.new_val.u8 = merged;
				return;
			}
			// Generic guard: if a wider entry already exists at this address,
			// skip the narrower write — the existing entry covers the range.
			if (typeSize(w->second.type) >= sz_bytes) {
				return;
			}
		}
	}

	// Also guard against the offset case: a write at byte-offset within an
	// 8-byte word where a wider entry exists at the aligned base address.
	if (bo.offset != 0) {
		void *base_addr = reinterpret_cast<void *>(bo.base_addr);
		auto w2 = tx->write_set.find(base_addr);
		if (w2 != tx->write_set.end() && w2->second.type == ValueType::UINT64) {
			unsigned shift = static_cast<unsigned>(bo.offset) * 8;
			uint64_t mask;
			uint64_t write_val;
			switch (sz) {
			case ValueType::UINT8:
				mask = (uint64_t)0xFF << shift;
				write_val = val.u1;
				break;
			case ValueType::UINT16:
				mask = (uint64_t)0xFFFF << shift;
				write_val = val.u2;
				break;
			case ValueType::UINT32:
				mask = (uint64_t)0xFFFFFFFF << shift;
				write_val = val.u4;
				break;
			default:
				mask = 0;
				write_val = 0;
				break;
			}
			if (mask) {
				uint64_t merged = (w2->second.new_val.u8 & ~mask);
				merged |= (write_val << shift);
				w2->second.new_val.u8 = merged;
				return;
			}
		}
	}

	Lock *lock = &g_locks_wbetl.get(bo.base_addr);

	if (lock->is_locked()) {
		if (lock->get_owner() != tx->id) {
			// Soft-spin (token as tie-breaker for deadlocks)
			if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 3)) {
				while (lock->is_locked() && lock->get_owner() != tx->id) {
					TINY_STM_PAUSE();
				}
				if (lock->get_owner() != tx->id) {
					if (!lock->try_lock(tx->id))
						abort_tx();
				}
				goto acquired_or_self;
			}
			tx->aborted = true;
			return;
		}
	} else {
		if (!lock->try_lock(tx->id)) {
			if (!validate())
				abort_tx();
			if (!lock->try_lock(tx->id)) {
				if (stm::tm_token_soft_spin(tx->abort_count, tx->id, 3)) {
					while (lock->is_locked()) {
						TINY_STM_PAUSE();
					}
					if (!lock->try_lock(tx->id))
						abort_tx();
				} else {
					tx->aborted = true;
					return;
				}
			}
		}
	}

acquired_or_self:
	// locked — deduplicate locks_held
	{
		bool already_held = false;
		for (Lock *hl : tx->locks_held) {
			if (hl == lock) { already_held = true; break; }
		}
		if (!already_held) {
			tx->locks_held.push_back(lock);
		}
	}

	WriteLogEntry_wbetl w;
	w.new_val = val;
	w.type = sz;
	tx->write_set[addr] = w;
}

inline uint8_t    //
tm_read_i1(       //
    uint8_t *addr //
)
{
	return tm_read<uint8_t,
	               ValueType::UINT8,
	               ReadLogEntry_wbetl,
	               WriteLogEntry_wbetl,
	               read_word_etl>(current_tx_wbetl, addr);
}

inline uint16_t    //
tm_read_i2(        //
    uint16_t *addr //
)
{
	return tm_read<uint16_t,
	               ValueType::UINT16,
	               ReadLogEntry_wbetl,
	               WriteLogEntry_wbetl,
	               read_word_etl>(current_tx_wbetl, addr);
}

inline uint32_t    //
tm_read_i4(        //
    uint32_t *addr //
)
{
	return tm_read<uint32_t,
	               ValueType::UINT32,
	               ReadLogEntry_wbetl,
	               WriteLogEntry_wbetl,
	               read_word_etl>(current_tx_wbetl, addr);
}

inline uint64_t    //
tm_read_i8(        //
    uint64_t *addr //
)
{
	return tm_read<uint64_t,
	               ValueType::UINT64,
	               ReadLogEntry_wbetl,
	               WriteLogEntry_wbetl,
	               read_word_etl>(current_tx_wbetl, addr);
}

inline float    //
tm_read_f4(     //
    float *addr //
)
{
	return tm_read<float,
	               ValueType::FLOAT,
	               ReadLogEntry_wbetl,
	               WriteLogEntry_wbetl,
	               read_word_etl>(current_tx_wbetl, addr);
}

inline double    //
tm_read_f8(      //
    double *addr //
)
{
	return tm_read<double,
	               ValueType::DOUBLE,
	               ReadLogEntry_wbetl,
	               WriteLogEntry_wbetl,
	               read_word_etl>(current_tx_wbetl, addr);
}

inline void *   //
tm_read_ptr(    //
    void **addr //
)
{
	return tm_read<void *,
	               ValueType::POINTER,
	               ReadLogEntry_wbetl,
	               WriteLogEntry_wbetl,
	               read_word_etl>(current_tx_wbetl, addr);
}

inline void        //
tm_write_i1(       //
    uint8_t *addr, //
    uint8_t val    //
)
{
	tm_write<uint8_t,
	         ValueType::UINT8,
	         ReadLogEntry_wbetl,
	         WriteLogEntry_wbetl,
	         write_word_etl>(current_tx_wbetl, addr, val);
}

inline void         //
tm_write_i2(        //
    uint16_t *addr, //
    uint16_t val    //
)
{
	tm_write<uint16_t,
	         ValueType::UINT16,
	         ReadLogEntry_wbetl,
	         WriteLogEntry_wbetl,
	         write_word_etl>(current_tx_wbetl, addr, val);
}

inline void         //
tm_write_i4(        //
    uint32_t *addr, //
    uint32_t val    //
)
{
	tm_write<uint32_t,
	         ValueType::UINT32,
	         ReadLogEntry_wbetl,
	         WriteLogEntry_wbetl,
	         write_word_etl>(current_tx_wbetl, addr, val);
}

inline void         //
tm_write_i8(        //
    uint64_t *addr, //
    uint64_t val    //
)
{
	tm_write<uint64_t,
	         ValueType::UINT64,
	         ReadLogEntry_wbetl,
	         WriteLogEntry_wbetl,
	         write_word_etl>(current_tx_wbetl, addr, val);
}

inline void      //
tm_write_f4(     //
    float *addr, //
    float val    //
)
{
	tm_write<float,
	         ValueType::FLOAT,
	         ReadLogEntry_wbetl,
	         WriteLogEntry_wbetl,
	         write_word_etl>(current_tx_wbetl, addr, val);
}

inline void       //
tm_write_f8(      //
    double *addr, //
    double val    //
)
{
	tm_write<double,
	         ValueType::DOUBLE,
	         ReadLogEntry_wbetl,
	         WriteLogEntry_wbetl,
	         write_word_etl>(current_tx_wbetl, addr, val);
}

inline void      //
tm_write_ptr(    //
    void **addr, //
    void *val    //
)
{
	tm_write<void *,
	         ValueType::POINTER,
	         ReadLogEntry_wbetl,
	         WriteLogEntry_wbetl,
	         write_word_etl>(current_tx_wbetl, addr, val);
}

} // namespace tinystm
