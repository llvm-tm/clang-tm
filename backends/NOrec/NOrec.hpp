#pragma once

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tm_common.hpp"

#ifndef NDEBUG
#define NOREC_ASSERT(cond, msg)                                                          \
	do {                                                                                 \
		if (!(cond)) {                                                                   \
			fprintf(stderr,                                                              \
			        "NOREC ASSERTION FAILED: %s (%s:%d)\n",                              \
			        msg,                                                                 \
			        __FILE__,                                                            \
			        __LINE__);                                                           \
			fflush(stderr);                                                              \
			assert(cond);                                                                \
		}                                                                                \
	} while (0)

#define NOREC_ASSERT_VALID_TX(tx, msg)                                                   \
	NOREC_ASSERT((tx) != nullptr, msg);                                                  \
	NOREC_ASSERT((tx)->active, "Transaction must be active: " msg);                      \
	NOREC_ASSERT(!(tx)->aborted, "Transaction must not be aborted: " msg)
#else                                  /* !NDEBUG */
#define NOREC_ASSERT(cond, msg)        /* EMPTY */
#define NOREC_ASSERT_VALID_TX(tx, msg) /* EMPTY */
#endif                                 /* NDEBUG */

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
	NOREC_ASSERT(tx != nullptr, "tx is null");
	NOREC_ASSERT(tx->active, "tx not active");

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
	NOREC_ASSERT(tx != nullptr, "tx is null");
	NOREC_ASSERT(tx->active, "tx not active");

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
	NOREC_ASSERT(tx, "tx not defined");

	do { tx->snapshot = get_clock(); } while (tx->snapshot & 1);
	tx->active = true;
	tx->read_only = true;
	tx->abort_count = 0;
	tx->read_set.clear();
	tx->write_set.clear();

	return true;
}

inline void //
abort_tx()  //
{
	auto *tx = current_tx;

	NOREC_ASSERT(tx, "tx not defined");
	NOREC_ASSERT(tx->active, "tx not active");

	tx->abort_count++;
	tx->clear();
	siglongjmp(*jmpbuf, 1);
	NOREC_ASSERT(false, "Did not jump");
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

	NOREC_ASSERT(tx, "tx not defined");
	NOREC_ASSERT(tx->active, "tx not active");

	if (tx->read_only) {
		tx->reset();
		return;
	}

	word_t expect = tx->snapshot;
	word_t desire = tx->snapshot + 1;
	NOREC_ASSERT((expect & 1) == 0, "Already locked");
	while (!global_lock.compare_exchange_strong(expect, desire)) {
		tx->snapshot = validate();
		expect = tx->snapshot;
		desire = expect + 1;
	}

	for (auto &w : tx->write_set) {
		write_value_to_addr(w.addr, w.new_val, w.type);
	}

	// After successful CAS from expect → expect+1:
	// - global_lock is now expect+1 (odd, lock held)
	// - Release by setting to expect+2 (next even version)
	set_clock(expect + 2);

	tx->reset();
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
	NOREC_ASSERT(tx, "tx not defined");
	NOREC_ASSERT(tx->active, "tx not active");

	for (auto &w : tx->write_set) {
		if (w.type == sz && w.addr == addr) {
			return w.new_val;
		}
	}

	any_type_t value = read_value_from_addr(addr, sz);

	while (tx->snapshot != get_clock()) {
		tx->snapshot = validate();
		value = read_value_from_addr(addr, sz);
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
	NOREC_ASSERT(tx, "tx not defined");
	NOREC_ASSERT(tx->active, "tx not active");

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

	// If a wider (or equal-width) entry already covers this address, skip.
	for (auto &w : tx->write_set) {
		if (w.addr == addr) {
			if (typeSize(w.type) >= sz_bytes) {
				return; // existing wider entry covers this address
			}
		}
	}

	WriteLogEntry w; // Create a new entry in write set
	w.new_val = val; // new val to write-back on commit
	w.type = sz;
	w.addr = addr;
	tx->write_set.push_back(w);
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
