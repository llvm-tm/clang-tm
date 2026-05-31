#pragma once

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tm_common.hpp"
#include "tm_log_entries.hpp"
#include "tm_log_merge.hpp"

// Assertions: use TM_ASSERT / TM_ASSERT_VALID_TX from tm_common.hpp.

namespace norec
{

constexpr const char *VERSION = "0.2.0";

using word_t = uint64_t;
using stm::any_to_u64;
using stm::any_type_mapping;
using stm::any_type_t;
using stm::ByteOffset;
using stm::fill_any_type;
using stm::read_value_from_addr;
using stm::return_any_type;
using stm::type_size;
using stm::u64_to_any;
using stm::ValueType;
using stm::write_value_to_addr;

struct ReadLogEntry : stm::BitmapReadEntry<void *> {};

struct WriteLogEntry : stm::BitmapRedoEntry<void *> {};

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

	return true;
}

inline void //
abort_tx()  //
{
	auto *tx = current_tx;

	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	tx->abort_count++;
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
			for (unsigned byte_off = 0; byte_off < 8; byte_off++) {
				if (r.valid & (1 << byte_off)) {
					void *byte_addr =
					    reinterpret_cast<void *>((uintptr_t)r.addr + byte_off);
					uint8_t mem_byte = 0;
					memcpy(&mem_byte, byte_addr, 1);
					uint8_t exp_byte =
					    static_cast<uint8_t>((r.value >> (byte_off * 8)) & 0xFF);
					if (mem_byte != exp_byte)
						abort_tx();
				}
			}
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

	for (auto &w : tx->write_set) {
		for (unsigned byte_off = 0; byte_off < 8; byte_off++) {
			if (w.valid & (1 << byte_off)) {
				void *byte_addr =
				    reinterpret_cast<void *>((uintptr_t)w.addr + byte_off);
				uint8_t byte_val =
				    static_cast<uint8_t>((w.value >> (byte_off * 8)) & 0xFF);
				write_value_to_addr(byte_addr,
				                    any_type_t{.u1 = byte_val},
				                    ValueType::UINT8);
			}
		}
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
	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	void *aligned = stm::merge::align_down_8(addr);

	for (auto it = tx->write_set.rbegin(); it != tx->write_set.rend(); ++it) {
		if (it->addr == aligned) {
			any_type_t result;
			if (stm::merge::bitmap_read(result, it->value, it->valid, sz, addr))
				return result;
			break;
		}
	}

	any_type_t value = read_value_from_addr(addr, sz);

	while (tx->snapshot != get_clock()) {
		tx->snapshot = validate();
		value = read_value_from_addr(addr, sz);
	}

	auto r_it = std::find_if(tx->read_set.rbegin(), tx->read_set.rend(),
	                         [aligned](const ReadLogEntry &e) {
	                             return e.addr == aligned;
	                         });
	if (r_it != tx->read_set.rend()) {
		stm::merge::merge_read(*r_it, tx->snapshot, value, sz, addr);
	} else {
		tx->read_set.push_back(stm::merge::make_read_entry<ReadLogEntry>(aligned, tx->snapshot, value, sz, addr));
	}

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

	tx->read_only = false;

	void *aligned = stm::merge::align_down_8(addr);

	for (auto it = tx->write_set.rbegin(); it != tx->write_set.rend(); ++it) {
		if (it->addr == aligned) {
			stm::merge::bitmap_write(it->value, it->valid, val, sz, addr);
			return;
		}
	}

	tx->write_set.push_back(stm::merge::make_write_entry<WriteLogEntry>(aligned, val, sz, addr));
}

/** -------------------------------------------------------
  * Stubs for Transaction read/write instrumentation.
  * ---------------------------------------------------- */

#define TM_STUB_TX current_tx
#define TM_STUB_READ_FN read_word_norec
#define TM_STUB_WRITE_FN write_word_norec
#include "../tm_stubs.hpp"

} // namespace norec
