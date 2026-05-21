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
			word_t locked = old & LOCK_MASK;
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
		return (state.load(std::memory_order_acquire) & LOCK_MASK) != 0;
	}

	word_t get_owner() const
	{
		return state.load(std::memory_order_acquire) & ~LOCK_MASK;
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

inline bool //
begin()     //
{
	auto *tx = current_tx_wbetl;

	TINYSTM_ASSERT(tx, "tx not defined");
	if (tx->active)
		return true;

	tx->start_version = get_clock();
	tx->end_version = tx->start_version;
	tx->active = true;
	tx->read_only = true;
	tx->abort_count = 0;

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

		if (lock->is_locked() || lock->get_version() > r.observed_version) {
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
		for (auto &it : tx->write_set) {
			auto &addr = it.first;
			auto &w = it.second;
			ByteOffset bo((word_t)addr);
			Lock *lock = &g_locks_wbetl.get(bo.base_addr);
			lock->unlock_with_version(tx->id, commit_version);
		}
	}

	tx->clear();
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

	Lock *lock = &g_locks_wbetl.get(bo.base_addr);
	if (lock->is_locked_by(tx->id)) {
		return read_value_from_addr(addr, sz);
	}

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

	tx->read_only = false; // TODO: shouldn't the TX abort?

	// Found write-set entry at exact addr with matching type → update in place.
	{
		auto w = tx->write_set.find(addr);
		if (w != tx->write_set.end()) {
			if (w->second.type == sz) {
				w->second.new_val = val;
				return;
			}
			// Type mismatch at same addr: merge sub-word write into wider entry.
			if (w->second.type == ValueType::UINT64 && sz == ValueType::UINT8) {
				uint64_t merged = (w->second.new_val.u8 & ~(uint64_t)0xFF);
				merged |= (uint64_t)(val.u1);
				w->second.new_val.u8 = merged;
				return;
			}
		}
	}

	// For sub-word writes at an offset within an 8-byte word: if the aligned
	// address already has a UINT64 entry, merge this write into it.
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

	while (true) {
		Lock *lock = &g_locks_wbetl.get(bo.base_addr);

		if (lock->is_locked() && lock->get_owner() != tx->id) {
			if (!validate())
				abort_tx();
			continue;
		}

		while (!lock->is_locked_by(tx->id) && !lock->try_lock(tx->id)) {
			if (!validate())
				abort_tx();
		}

		// locked

		tx->locks_held.push_back(lock);

		WriteLogEntry_wbetl w;
		w.new_val = val;
		w.type = sz;
		tx->write_set.insert(std::pair(addr, w));
		break;
	}
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
