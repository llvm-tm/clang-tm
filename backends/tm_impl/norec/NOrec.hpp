#pragma once

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tm_common.hpp"
#include "tm_event_logger.hpp"

// NOrec uses shared TM_ASSERT / TM_ASSERT_VALID_TX from tm_common.hpp.
#ifndef NDEBUG
#define NOREC_ASSERT_VALID_TX(tx, msg)                                                   \
	TM_ASSERT((tx) != nullptr, msg);                                                     \
	TM_ASSERT((tx)->active, "Transaction must be active: " msg);                         \
	TM_ASSERT(!(tx)->aborted, "Transaction must not be aborted: " msg)
#else
#define NOREC_ASSERT_VALID_TX(tx, msg) /* EMPTY */
#endif

namespace norec
{

constexpr const char *VERSION = "0.2.0";

using word_t = uint64_t;
using stm::any_type_mapping;
using stm::any_type_t;
using stm::fill_any_type;

using stm::read_value_from_addr;
using stm::return_any_type;
using stm::ValueType;
using stm::write_value_to_addr;

extern std::atomic<uint64_t> g_tm_abort_count;

struct ReadLogEntry {
	ValueType type;
	void *addr;
	volatile word_t observed_version;
	any_type_t observed_val;
};

struct WriteLogEntry {
	ValueType type;
	void *addr;
	any_type_t new_val;
};

class Transaction
{
public:
	volatile word_t id = 0;
	volatile word_t snapshot = 0;
	bool active = false;
	bool read_only = true;
	int abort_count = 0;
	std::vector<ReadLogEntry> read_set;
	std::vector<WriteLogEntry> write_set;

	void reset()
	{
		snapshot = 0;
		active = false;
		read_only = true;
		abort_count = 0;
		clear();
	}

	void clear()
	{
		read_set.clear();
		write_set.clear();
	}
};

extern __thread sigjmp_buf *jmpbuf;
extern __thread Transaction *current_tx;
extern __thread sigjmp_buf *jmpbuf;

static void setjmp(sigjmp_buf *buf) { jmpbuf = buf; }

inline bool          //
iseq(                //
    any_type_t val1, //
    any_type_t val2  //
)
{
	return val1.u8 == val2.u8;
}

template <typename T,
          ValueType SZ,
          any_type_t (*read_word)(Transaction *,
                                  void *,
                                  ValueType)>
inline T tm_read(    //
    Transaction *tx, //
    T *addr          //
)
{
	TM_ASSERT(tx != nullptr, "tx is null");
	TM_ASSERT(tx->active, "tx not active");

	if (!stm::isTMAddress(addr)) {
		return *addr;
	}

	any_type_t r = read_word(tx, (void *)addr, SZ);
	return return_any_type<T>(r);
}

template <typename T,
          ValueType SZ,
          void (*write_word)(Transaction *,
                             void *,
                             any_type_t,
                             ValueType)>
inline void          //
tm_write(            //
    Transaction *tx, //
    T *addr,         //
    T val            //
)
{
	TM_ASSERT(tx != nullptr, "tx is null");
	TM_ASSERT(tx->active, "tx not active");

	if (!stm::isTMAddress(addr)) {
		*addr = val;
		return;
	}

	any_type_t w;
	fill_any_type(w, &val, SZ);
	write_word(tx, (void *)addr, w, SZ);
}

extern std::atomic<word_t> global_lock;
extern std::atomic<norec::word_t> thr_counter;
extern std::atomic<norec::word_t> reset_locks_thr;

inline void init()
{
	global_lock.store(0, std::memory_order_release);
	thr_counter.store(1, std::memory_order_release);
}

inline void exit()
{
	// TODO: free shared structures
}

inline word_t get_clock() { return global_lock.load(std::memory_order_acquire); }
inline void set_clock(word_t clock)
{
	global_lock.store(clock, std::memory_order_release);
}

struct ReadLogEntry_wbctl {
	ValueType type;
	void *addr;
	volatile word_t observed_version;
	any_type_t observed_val;
};

struct WriteLogEntry_wbctl {
	ValueType type;
	void *addr;
	any_type_t new_val;
};

/** -------------------------------------------------------
  * Stubs for initialization/destruction.
  * ---------------------------------------------------- */

inline void   //
init_thread() //
{
	if (!current_tx) {
		current_tx = new Transaction();
	}
	current_tx->id = thr_counter.fetch_add(1, std::memory_order_acq_rel);
	current_tx->reset();
}

inline void   //
exit_thread() //
{
	delete current_tx;
	current_tx = nullptr;
}

/** -------------------------------------------------------
  * Stubs for Transaction begin/end.
  * ---------------------------------------------------- */

inline bool //
begin()     //
{
	auto *tx = current_tx;
	TM_ASSERT(tx, "tx not defined");

	do { tx->snapshot = get_clock(); } while (tx->snapshot & 1);
	tx->active = true;
	tx->read_only = true;
	tx->abort_count = 0;
	tx->read_set.clear();
	tx->write_set.clear();

	TM_EVENT(TX_BEGIN, tx->id, tx->snapshot);
	return true;
}

inline void //
abort_tx()  //
{
	auto *tx = current_tx;

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	TM_EVENT(TX_ABORT, tx->id, tx->abort_count);
	tx->abort_count++;
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
	tx->clear();
	siglongjmp(*jmpbuf, 1);
	TM_ASSERT(false, "Did not jump");
}

inline __attribute__((noinline)) word_t //
validate()    //
{
	auto *tx = current_tx;
	volatile word_t time = 0;
	while (true) {
		time = get_clock();
		if ((time & 1) != 0)
			continue;
		if (tx->read_set.empty())
			break;
		for (auto &r : tx->read_set) {
			any_type_t check_val = read_value_from_addr(r.addr, r.type);
			if (!iseq(check_val, r.observed_val))
				abort_tx();
		}
		// Only check after validating the ENTIRE read set, not after each read
		if (time == get_clock())
			return time;
		// Lock changed during validation - restart from beginning
	}
	return time;
}

inline void //
commit()    //
{
	auto *tx = current_tx;
	volatile word_t commit_version = 0;

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	if (tx->read_only) {
		tx->reset();
		return;
	}

	word_t expect = tx->snapshot;
	word_t desire = tx->snapshot + 1;
	TM_ASSERT((expect & 1) == 0, "Already locked");
	while (!global_lock.compare_exchange_strong(expect, desire)) {
		tx->snapshot = validate();
		expect = tx->snapshot;
		desire = expect + 1;
	}
	TM_EVENT2(COMMIT_LOCK_ACQUIRE, expect, desire, 0);

	for (auto &w : tx->write_set) {
		TM_EVENT2(COMMIT_WRITEBACK, (word_t)w.addr, (word_t)w.type, 0);
		write_value_to_addr(w.addr, w.new_val, w.type);
	}

	// After successful CAS from expect → expect+1:
	// - global_lock is now expect+1 (odd, lock held)
	// - Release by setting to expect+2 (next even version)
	set_clock(expect + 2);

	tx->reset();
	TM_EVENT2(COMMIT_SUCCESS, tx->id, expect + 2, 0);
	return;
}

/** -------------------------------------------------------
  * Stubs for Transaction read/write instrumentation.
  * ---------------------------------------------------- */

inline __attribute__((noinline)) any_type_t    //
read_word_norec(     //
    Transaction *tx, //
    void *addr,      //
    ValueType sz     //
)
{
	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	// Stack-address detection: stack data is thread-private.  Reading via
	// tm_read would either return stale values (if write-back and no write-
	// set entry) or create spurious read-set entries that cause validation
	// failures (lock-based backends hash stack addresses to random locks).

	// Scan from the END (most recent write) so that addresses written
	// multiple times (e.g., vector _M_finish on each push_back) return
	// the latest value, not the oldest.
	for (auto it = tx->write_set.rbegin(); it != tx->write_set.rend(); ++it) {
		if (it->type == sz && it->addr == addr) {
			if (sz == ValueType::POINTER) {
				uint64_t ptr_val = reinterpret_cast<uint64_t>(it->new_val.ptr);
				assert(ptr_val < 0x800000000000ULL &&
				       "read_word_norec: POINTER from write-set is kernel-space");
			}
			return it->new_val;
		}
	}

	// Type-interchange fallback: scan for same-address entries with compatible
	// types.  The 8-byte memcpy/memmove expansion produces UINT64 entries,
	// but subsequent POINTER reads need POINTER↔UINT64 interchange.
	// Also handles wider-to-narrower extraction (UINT64→UINT8/16/32) and
	// byte-level merge (8× UINT8 entries → POINTER/UINT64).
	for (auto it = tx->write_set.rbegin(); it != tx->write_set.rend(); ++it) {
		if (it->addr != addr) continue;

		auto entrySize = [](ValueType t) -> unsigned {
			switch (t) {
			case ValueType::UINT8:   return 1;
			case ValueType::UINT16:  return 2;
			case ValueType::UINT32:
			case ValueType::FLOAT:   return 4;
			case ValueType::UINT64:
			case ValueType::DOUBLE:
			case ValueType::POINTER: return 8;
			default:                 return 0;
			}
		};
		unsigned es = entrySize(it->type);
		unsigned rs = entrySize(sz);

		// POINTER ↔ UINT64 interchange (both 8 bytes)
		if (rs == 8 && es == 8 && sz != it->type)
			return it->new_val;

		// Wider to narrower: extract sub-word from wider entry
		if (es == 8 && rs == 4 && (sz == ValueType::UINT32 || sz == ValueType::FLOAT)) {
			any_type_t r; r.u4 = (uint32_t)(it->new_val.u8 & 0xFFFFFFFF); return r;
		}
		if (es == 8 && rs == 2 && sz == ValueType::UINT16) {
			any_type_t r; r.u2 = (uint16_t)(it->new_val.u8 & 0xFFFF); return r;
		}
		if (es == 8 && rs == 1 && sz == ValueType::UINT8) {
			any_type_t r; r.u1 = (uint8_t)(it->new_val.u8 & 0xFF); return r;
		}
		if (es == 4 && rs == 2 && sz == ValueType::UINT16) {
			any_type_t r; r.u2 = (uint16_t)(it->new_val.u4 & 0xFFFF); return r;
		}
		if (es == 4 && rs == 1 && sz == ValueType::UINT8) {
			any_type_t r; r.u1 = (uint8_t)(it->new_val.u4 & 0xFF); return r;
		}
		if (es == 2 && rs == 1 && sz == ValueType::UINT8) {
			any_type_t r; r.u1 = (uint8_t)(it->new_val.u2 & 0xFF); return r;
		}
	}

	// General byte-merge: if a wider read (UINT64/POINTER) has no matching
	// write-set entry at the start address, check if all sub-bytes have
	// UINT8 entries and merge them.  This handles memcpy/memmove byte-loop
	// instrumentation that creates UINT8 entries at every byte offset.
	if (sz == ValueType::UINT64 || sz == ValueType::POINTER || sz == ValueType::DOUBLE) {
		uint64_t merged = 0;
		bool all_byte = true;
		for (unsigned i = 0; i < 8; i++) {
			void *byte_addr = (void*)((uintptr_t)addr + i);
			bool found = false;
			for (auto it = tx->write_set.rbegin(); it != tx->write_set.rend(); ++it) {
				if (it->addr == byte_addr && it->type == ValueType::UINT8) {
					merged |= ((uint64_t)it->new_val.u1) << (i * 8);
					found = true;
					break;
				}
			}
			if (!found) { all_byte = false; break; }
		}
		if (all_byte) {
			any_type_t result;
			result.u8 = merged;
			return result;
		}
	}

	any_type_t value = read_value_from_addr(addr, sz);

	if (sz == ValueType::POINTER) {
		uint64_t ptr_val = reinterpret_cast<uint64_t>(value.ptr);
		assert(ptr_val < 0x800000000000ULL &&
		       "read_word_norec: POINTER from memory is kernel-space");
	}

	while (tx->snapshot != get_clock()) {
		tx->snapshot = validate();
		value = read_value_from_addr(addr, sz);
		if (sz == ValueType::POINTER) {
			uint64_t ptr_val = reinterpret_cast<uint64_t>(value.ptr);
			assert(ptr_val < 0x800000000000ULL &&
			       "read_word_norec: POINTER re-read from memory is kernel-space");
		}
		if (tx->read_set.size() > 1000000) {
			fprintf(stderr, "FATAL: read_set overflow (%zu entries)\n", tx->read_set.size());
			abort_tx();
		}
	}

	ReadLogEntry r;
	r.addr = addr;
	r.observed_val = value;
	r.observed_version = tx->snapshot;
	r.type = sz;
	tx->read_set.push_back(r);

	TM_EVENT(READ_LOCK_ACQUIRE, (word_t)addr, tx->snapshot);
	return value;
}

inline __attribute__((noinline)) void  //
write_word_norec(    //
    Transaction *tx, //
    void *addr,      //
    any_type_t val,  //
    ValueType sz     //
)
{
	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	tx->read_only = false; // TODO: shouldn't the TX abort?

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

	// Assert: POINTER values must be in user space (below 0x800000000000)
	if (sz == ValueType::POINTER) {
		uint64_t ptr_val = reinterpret_cast<uint64_t>(val.ptr);
		assert(ptr_val < 0x800000000000ULL && "write_word_norec: POINTER value in kernel space");
	}

	// Scan from end (most recent) so repeated writes to the same address
	// update the existing entry rather than being silently dropped.
	// (e.g., vector _M_finish updated on every push_back)
	for (auto it = tx->write_set.rbegin(); it != tx->write_set.rend(); ++it) {
		if (it->addr == addr) {
			if (typeSize(it->type) == sz_bytes) {
				// Assert: when updating a POINTER entry with a same-size non-POINTER type,
				// the new value might be a non-pointer value. This is only safe if the
				// non-POINTER value is a valid user-space address (for type interchange).
				if (it->type == ValueType::POINTER && sz_bytes == 8 && sz != ValueType::POINTER) {
					assert(val.u8 < 0x800000000000ULL &&
					       "write_word_norec: non-POINTER update to POINTER entry got kernel-space value");
				}
				it->new_val = val; // same type: update most recent entry
				return;
			}
			if (typeSize(it->type) > sz_bytes) {
				return; // wider entry covers this address — skip
			}
		}
	}

	WriteLogEntry w; // Create a new entry in write set
	w.new_val = val; // new val to write-back on commit
	w.type = sz;
	w.addr = addr;
	tx->write_set.push_back(w);
	TM_EVENT2(WRITE_SET_INSERT, (word_t)addr, (word_t)sz, 0);
}

/** -------------------------------------------------------
  * Stubs for Transaction read/write instrumentation.
  * (Type aware: they just call the functions above)
  * ---------------------------------------------------- */

inline uint8_t    //
tm_read_i1(       //
    uint8_t *addr //
)
{
	return tm_read<uint8_t, ValueType::UINT8, read_word_norec>(current_tx, addr);
}

inline uint16_t    //
tm_read_i2(        //
    uint16_t *addr //
)
{
	return tm_read<uint16_t, ValueType::UINT16, read_word_norec>(current_tx, addr);
}

inline uint32_t    //
tm_read_i4(        //
    uint32_t *addr //
)
{
	return tm_read<uint32_t, ValueType::UINT32, read_word_norec>(current_tx, addr);
}

inline uint64_t    //
tm_read_i8(        //
    uint64_t *addr //
)
{
	return tm_read<uint64_t, ValueType::UINT64, read_word_norec>(current_tx, addr);
}

inline float    //
tm_read_f4(     //
    float *addr //
)
{
	return tm_read<float, ValueType::FLOAT, read_word_norec>(current_tx, addr);
}

inline double    //
tm_read_f8(      //
    double *addr //
)
{
	return tm_read<double, ValueType::DOUBLE, read_word_norec>(current_tx, addr);
}

inline void *   //
tm_read_ptr(    //
    void **addr //
)
{
	return tm_read<void *, ValueType::POINTER, read_word_norec>(current_tx, addr);
}

inline void        //
tm_write_i1(       //
    uint8_t *addr, //
    uint8_t val    //
)
{
	tm_write<uint8_t, ValueType::UINT8, write_word_norec>(current_tx, addr, val);
}

inline void         //
tm_write_i2(        //
    uint16_t *addr, //
    uint16_t val    //
)
{
	tm_write<uint16_t, ValueType::UINT16, write_word_norec>(current_tx, addr, val);
}

inline void         //
tm_write_i4(        //
    uint32_t *addr, //
    uint32_t val    //
)
{
	tm_write<uint32_t, ValueType::UINT32, write_word_norec>(current_tx, addr, val);
}

inline void         //
tm_write_i8(        //
    uint64_t *addr, //
    uint64_t val    //
)
{
	tm_write<uint64_t, ValueType::UINT64, write_word_norec>(current_tx, addr, val);
}

inline void      //
tm_write_f4(     //
    float *addr, //
    float val    //
)
{
	tm_write<float, ValueType::FLOAT, write_word_norec>(current_tx, addr, val);
}

inline void       //
tm_write_f8(      //
    double *addr, //
    double val    //
)
{
	tm_write<double, ValueType::DOUBLE, write_word_norec>(current_tx, addr, val);
}

inline void      //
tm_write_ptr(    //
    void **addr, //
    void *val    //
)
{
	tm_write<void *, ValueType::POINTER, write_word_norec>(current_tx, addr, val);
}

} // namespace norec
