/**
 * TinySTM - WRITE_BACK_CTL (Commit-Time Locking)
 */

#pragma once

#include <cstring>
#include <thread>
#include <vector>

#include "tinystm_common.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
}

namespace tinystm
{

constexpr const char *VERSION = "0.2.0-wbctl";

struct ReadLogEntry_wbctl {
	ValueType type;
	void *addr;
	volatile word_t observed_version;
	any_type_t observed_val;
};

struct WriteLogEntry_wbctl {
	ValueType type;
	void *addr;
	word_t version;
	any_type_t new_val;
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
	}
	current_tx_wbctl->id = thr_counter.fetch_add(1, std::memory_order_acq_rel);
	current_tx_wbctl->reset();
}

inline void   //
exit_thread() //
{
	delete current_tx_wbctl;
	current_tx_wbctl = nullptr;
}

static void //
init_rand() //
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
	std::exponential_distribution<> dist((double)1 /
	                                     (double)(current_tx_wbctl->abort_count + 1));
	int delay = std::min(dist(rng), 1e5); // max delay is 100ms
	// printf("THR%llu Sleep for %i\n", current_tx_wbctl->id, delay);
	std::this_thread::sleep_for(std::chrono::microseconds(delay));
}

/** -------------------------------------------------------
  * Stubs for Transaction begin/end.
  * ---------------------------------------------------- */

inline bool //
begin()     //
{
	auto *tx = current_tx_wbctl;

	// printf("THR%llu begin: tx->abort_count=%i tm_nested_call_counter=%i\n",
	//        tx->id,
	//        tx->abort_count,
	//        tm_nested_call_counter);

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(!tx->active, "nested not supported");

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
	auto *tx = current_tx_wbctl;

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	tx->unlock_held_locks_and_clear();
	tx->abort_count++;
	fprintf(stderr, "TinySTM abort_tx #%d\n", tx->abort_count);
	fflush(stderr);
	// printf("THR%llu abort_tx (%i)\n", tx->id, tx->abort_count);
	// std::atomic_thread_fence(std::memory_order_acq_rel);
	if (tx->abort_count > 5) { // Magic number
		// random backoff when aborts are really bad
		random_backoff();
	}
	siglongjmp(*jmpbuf, 1);
	TINYSTM_ASSERT(false, "Did not jump");
}

inline bool //
validate()  //
{
	auto *tx = current_tx_wbctl;
	for (auto &r : tx->read_set) {
		ByteOffset bo((word_t)r.addr);
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

static bool                       //
compareByAddr(                    //
    const WriteLogEntry_wbctl &a, //
    const WriteLogEntry_wbctl &b  //
)
{
	return (word_t)a.addr < (word_t)b.addr;
}

inline bool //
commit()    //
{
	auto *tx = current_tx_wbctl;
	volatile word_t commit_version = 0;

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	if (!tx->read_only) { // Acquire locks and write-back

		// Lock acquisition phase
		if (tx->abort_count > 2) { // Magic number
			std::sort(tx->write_set.begin(), tx->write_set.end(), compareByAddr);
		}
		for (auto &w : tx->write_set) {
			ByteOffset bo((word_t)w.addr);
			Lock *lock = &g_locks_wbctl.get(bo.base_addr);
			volatile word_t l = lock->get();
			word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
			if (owner != tx->id) {                // skip self-locks
				while (!lock->try_lock(tx->id)) { // if lock is busy...
					if (!extend()) {              // ... try to validate the read-set...
						abort_tx();               // ... then, if fails, return to begin
					}
				}
				tx->locks_held.push_back(lock); // keep track of locks
			}
			TINYSTM_ASSERT(lock->is_locked() && lock->get_owner() == tx->id,
			               "Lock not locked or wrong owner");
		}

		// can commit, increase the global clock
		if ((commit_version = increment_clock(tx->id)) < tx->end_version)
			abort_tx(); // version overflow

		// Check if there were transactions in between
		if (commit_version != tx->end_version + 1) {
			if (!extend()) {
				abort_tx(); // can leave gaps in the global clock
			}
		}

		// increment_clock(tx->id); // Reading above and incrementing here does not work

		// Write-back phase
		for (auto &w : tx->write_set) {
			ByteOffset bo((word_t)w.addr);
			Lock *lock = &g_locks_wbctl.get(bo.base_addr);
			// TINYSTM_ASSERT(lock->get_version() == w.version,
			//                "Version of owned lock changed");
			// TINYSTM_ASSERT(*(uint32_t *)w.addr == w.new_val.u4 - 1,
			//                "Version of owned lock changed");
			write_value_to_addr(w.addr, w.new_val, w.type);
		}

		for (auto &w : tx->write_set) {
			ByteOffset bo((word_t)w.addr);
			Lock *lock = &g_locks_wbctl.get(bo.base_addr);
			if (!lock->is_locked_by(tx->id)) {
				TINYSTM_ASSERT(false, "Write to unlocked position");
				continue; // possible duplicated writes
			}
			if (lock->get_version() > commit_version) {
				lock->unlock(tx->id);
			} else {
				TINYSTM_ASSERT(lock->get_version() <= commit_version,
				               "Lock version updated while locked");
				lock->unlock_with_version(tx->id, commit_version);
			}
			TINYSTM_ASSERT(lock->get_version() >= commit_version,
			               "Lock version not updated");
		}

		// clear lock list
		tx->locks_held.clear();
	}

	tx->reset();
	// printf("THR%llu commit, active=%i\n", tx->id, tx->active);
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
	ByteOffset bo((word_t)addr);

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	for (auto &w : tx->write_set) {
		ByteOffset bo2((word_t)w.addr);
		if (w.type == sz && same_location(bo2, bo)) {
			return w.new_val;
		}
	}

	Lock *lock = &g_locks_wbctl.get(bo.base_addr);
	TINYSTM_ASSERT(!lock->is_locked_by(tx->id), "wbctl locks at commit time");
	volatile word_t l = lock->get();

	for (auto &r : tx->read_set) {
		ByteOffset bo2((word_t)r.addr);
		if (r.type == sz && same_location(bo2, bo)) {
			word_t current_version = (l & (VERSION_MASK << META_BITS)) >> META_BITS;
			if (current_version > r.observed_version && !extend())
				abort_tx();
			return r.observed_val;
		}
	}

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

		ReadLogEntry_wbctl r;
		r.addr = addr;
		r.observed_version = version;
		r.observed_val = val;
		r.type = sz;
		tx->read_set.push_back(r);

		return val;
	}
}

inline void                                                   //
write_word_ctl(                                               //
    Transaction<ReadLogEntry_wbctl, WriteLogEntry_wbctl> *tx, //
    void *addr,                                               //
    any_type_t val,                                           //
    ValueType sz                                              //
)
{
	ByteOffset bo((word_t)addr);

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	tx->read_only = false; // TODO: shouldn't the TX abort?

	for (auto &w : tx->write_set) {
		ByteOffset bo2((word_t)w.addr);
		if (w.type == sz && same_location(bo2, bo)) {
			w.new_val = val;
			return;
		}
	}

	while (true) {
		Lock *lock = &g_locks_wbctl.get(bo.base_addr);
		volatile word_t l = lock->get();
		word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
		bool is_locked = (l & OWNED_MASK) != 0;

		TINYSTM_ASSERT(owner != tx->id, "WBCTL only locks at commit time");

		// If is locked, but the read set is valid, just continue
		if (is_locked && !validate()) {
			abort_tx(); // returns to begin
		}

		WriteLogEntry_wbctl w;          // Create a new entry in writeset
		tx->locks_held.push_back(lock); // saves the lock for later unlock
		w.new_val = val;                // new val to write-back on commit
		w.type = sz;
		w.addr = addr;
		w.version = lock->get_version();
		tx->write_set.push_back(w);
		return;
	}
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
	return tm_read<uint8_t,
	               ValueType::UINT8,
	               ReadLogEntry_wbctl,
	               WriteLogEntry_wbctl,
	               read_word_ctl>(current_tx_wbctl, addr);
}

inline uint16_t    //
tm_read_i2(        //
    uint16_t *addr //
)
{
	return tm_read<uint16_t,
	               ValueType::UINT16,
	               ReadLogEntry_wbctl,
	               WriteLogEntry_wbctl,
	               read_word_ctl>(current_tx_wbctl, addr);
}

inline uint32_t    //
tm_read_i4(        //
    uint32_t *addr //
)
{
	return tm_read<uint32_t,
	               ValueType::UINT32,
	               ReadLogEntry_wbctl,
	               WriteLogEntry_wbctl,
	               read_word_ctl>(current_tx_wbctl, addr);
}

inline uint64_t    //
tm_read_i8(        //
    uint64_t *addr //
)
{
	return tm_read<uint64_t,
	               ValueType::UINT64,
	               ReadLogEntry_wbctl,
	               WriteLogEntry_wbctl,
	               read_word_ctl>(current_tx_wbctl, addr);
}

inline float    //
tm_read_f4(     //
    float *addr //
)
{
	return tm_read<float,
	               ValueType::FLOAT,
	               ReadLogEntry_wbctl,
	               WriteLogEntry_wbctl,
	               read_word_ctl>(current_tx_wbctl, addr);
}

inline double    //
tm_read_f8(      //
    double *addr //
)
{
	return tm_read<double,
	               ValueType::DOUBLE,
	               ReadLogEntry_wbctl,
	               WriteLogEntry_wbctl,
	               read_word_ctl>(current_tx_wbctl, addr);
}

inline void *   //
tm_read_ptr(    //
    void **addr //
)
{
	return tm_read<void *,
	               ValueType::POINTER,
	               ReadLogEntry_wbctl,
	               WriteLogEntry_wbctl,
	               read_word_ctl>(current_tx_wbctl, addr);
}

inline void        //
tm_write_i1(       //
    uint8_t *addr, //
    uint8_t val    //
)
{
	tm_write<uint8_t,
	         ValueType::UINT8,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

inline void         //
tm_write_i2(        //
    uint16_t *addr, //
    uint16_t val    //
)
{
	tm_write<uint16_t,
	         ValueType::UINT16,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

inline void         //
tm_write_i4(        //
    uint32_t *addr, //
    uint32_t val    //
)
{
	tm_write<uint32_t,
	         ValueType::UINT32,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

inline void         //
tm_write_i8(        //
    uint64_t *addr, //
    uint64_t val    //
)
{
	tm_write<uint64_t,
	         ValueType::UINT64,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

inline void      //
tm_write_f4(     //
    float *addr, //
    float val    //
)
{
	tm_write<float,
	         ValueType::FLOAT,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

inline void       //
tm_write_f8(      //
    double *addr, //
    double val    //
)
{
	tm_write<double,
	         ValueType::DOUBLE,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

inline void      //
tm_write_ptr(    //
    void **addr, //
    void *val    //
)
{
	tm_write<void *,
	         ValueType::POINTER,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

} // namespace tinystm
