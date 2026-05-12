/**
 * TinySTM_new - WRITE_THROUGH Full Implementation per Paper Specification
 *
 * Features:
 * - Encounter-time locking
 * - Time-based design with global clock
 * - Versioned write-locks
 * - Double-check read protocol (lock → value → lock)
 * - Write-through strategy (direct writes to memory)
 * - Incarnation numbers for abort detection
 * - Multi-type support: 8, 16, 32, 64-bit integers, floats, doubles, pointers
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define TINYSTM_ASSERT(cond, msg)                                                        \
	do {                                                                                 \
		if (!(cond)) {                                                                   \
			fprintf(stderr,                                                              \
			        "TINYSTM ASSERTION FAILED: %s (%s:%d)\n",                            \
			        msg,                                                                 \
			        __FILE__,                                                            \
			        __LINE__);                                                           \
			fflush(stderr);                                                              \
			assert(cond);                                                                \
		}                                                                                \
	} while (0)

#define TINYSTM_ASSERT_VALID_TX(tx, msg)                                                 \
	TINYSTM_ASSERT((tx) != nullptr, msg);                                                \
	TINYSTM_ASSERT((tx)->active, "Transaction must be active: " msg);                    \
	TINYSTM_ASSERT(!(tx)->aborted, "Transaction must not be aborted: " msg)

namespace tinystm
{

constexpr const char *VERSION = "8.2.0-full-wt";

using word_t = uintptr_t;

constexpr unsigned LOCK_ARRAY_LOG_SIZE = 20;
constexpr unsigned LOCK_ARRAY_SIZE = 1 << LOCK_ARRAY_LOG_SIZE;
constexpr unsigned LOCK_EXTRA_BITS = 2;

constexpr unsigned INCARNATION_BITS = 3;
constexpr word_t LOCK_MASK = 1;
constexpr word_t INCARNATION_MASK = 0x7;
constexpr word_t VERSION_MASK = ~(LOCK_MASK | INCARNATION_MASK);

enum class ValueType : uint8_t {
	UINT8 = 1,
	UINT16 = 2,
	UINT32 = 4,
	UINT64 = 8,
	FLOAT = 16,
	DOUBLE = 32,
	POINTER = 64
};

struct ByteOffset {
	word_t base_addr;
	uint8_t offset;

	ByteOffset()
	    : base_addr(0),
	      offset(0)
	{
	}
	ByteOffset(word_t addr)
	    : base_addr(addr & ~7),
	      offset(addr & 7)
	{
	}
};

inline bool same_location(const ByteOffset &a, const ByteOffset &b)
{
	return a.base_addr == b.base_addr && a.offset == b.offset;
}

struct ReadLogEntry {
	word_t *lock_addr;
	ByteOffset location;
	word_t observed_version;
	word_t observed_incarnation;
	word_t observed_word;
	ValueType type;
};

struct WriteLogEntry {
	ByteOffset location;
	word_t old_word;
	word_t new_word;
	ValueType type;
	word_t version;
	word_t incarnation;
};

class Transaction
{
public:
	word_t start_version = 0;
	word_t end_version = 0;
	bool active = false;
	bool aborted = false;
	bool read_only = true;
	int nesting = 1;
	int abort_count = 0;
	std::vector<ReadLogEntry> read_set;
	std::vector<WriteLogEntry> write_set;
	std::vector<word_t *> locks_held;

	void reset()
	{
		start_version = 0;
		end_version = 0;
		active = false;
		aborted = false;
		read_only = true;
		nesting = 1;
		abort_count = 0;
		read_set.clear();
		write_set.clear();
		locks_held.clear();
	}
};

class LockTable
{
public:
	struct Lock {
		std::atomic<word_t> state{0};

		word_t get() const { return state.load(std::memory_order_acquire); }

		word_t get_version() const
		{
			return (state.load(std::memory_order_acquire) >>
			        (LOCK_EXTRA_BITS + INCARNATION_BITS));
		}

		word_t get_incarnation() const
		{
			return (state.load(std::memory_order_acquire) >> LOCK_EXTRA_BITS) &
			       INCARNATION_MASK;
		}

		bool is_locked() const
		{
			return (state.load(std::memory_order_acquire) & LOCK_MASK) != 0;
		}

		bool is_locked_by(word_t tx_id) const
		{
			word_t s = state.load(std::memory_order_acquire);
			return (s & LOCK_MASK) &&
			       ((s >> (LOCK_EXTRA_BITS + INCARNATION_BITS)) == tx_id);
		}

		bool try_lock(word_t tx_id, word_t incarnation)
		{
			word_t expected = 0;
			word_t desired = (tx_id << (LOCK_EXTRA_BITS + INCARNATION_BITS)) |
			                 (incarnation << LOCK_EXTRA_BITS) | LOCK_MASK;
			return state.compare_exchange_strong(expected,
			                                     desired,
			                                     std::memory_order_acquire,
			                                     std::memory_order_acquire);
		}

		void unlock(word_t new_version, word_t new_incarnation)
		{
			word_t desired = (new_version << (LOCK_EXTRA_BITS + INCARNATION_BITS)) |
			                 (new_incarnation << LOCK_EXTRA_BITS);
			state.store(desired, std::memory_order_release);
		}

		void inc_abort(word_t current_incarnation)
		{
			word_t current = state.load(std::memory_order_acquire);
			word_t new_incarnation = ((current >> LOCK_EXTRA_BITS) & INCARNATION_MASK) +
			                         1;
			if (new_incarnation > INCARNATION_MASK) {
				new_incarnation = 0;
			}
			word_t version = (current >> (LOCK_EXTRA_BITS + INCARNATION_BITS));
			word_t desired = (version << (LOCK_EXTRA_BITS + INCARNATION_BITS)) |
			                 (new_incarnation << LOCK_EXTRA_BITS);
			state.store(desired, std::memory_order_release);
		}
	};

	Lock locks[LOCK_ARRAY_SIZE];

public:
	Lock *get(word_t addr)
	{
		return &locks[(addr >> LOCK_EXTRA_BITS) & (LOCK_ARRAY_SIZE - 1)];
	}
};

typedef LockTable::Lock Lock;

static LockTable g_locks;
static std::atomic<word_t> g_clock{1};

thread_local Transaction *current_tx = nullptr;

inline word_t get_clock() { return g_clock.load(std::memory_order_acquire); }

inline word_t increment_clock()
{
	return g_clock.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline void init() { g_clock.store(1, std::memory_order_relaxed); }

inline void init_thread()
{
	if (!current_tx) {
		current_tx = new Transaction();
	}
	current_tx->reset();
}

inline void exit_thread()
{
	if (!current_tx) return;
	delete current_tx;
	current_tx = nullptr;
}

inline bool begin()
{
	init_thread();

	auto *tx = current_tx;
	TINYSTM_ASSERT(tx != nullptr, "begin: tx is null");

	fprintf(stderr, "TinySTM begin() called\n");
	fflush(stderr);

	if (tx->active) {
		if (tx->aborted) {
			for (auto &w : tx->write_set) {
				word_t *addr = (word_t *)w.location.base_addr;
				*addr = w.old_word;
			}
			for (word_t *lock_ptr : tx->locks_held) {
				Lock *lock = (Lock *)lock_ptr;
				lock->unlock(0, 0);
			}
			tx->locks_held.clear();
			tx->write_set.clear();
			tx->read_set.clear();
			tx->aborted = false;
			tx->start_version = get_clock();
			tx->end_version = tx->start_version;
			TINYSTM_ASSERT(tx->start_version > 0,
			               "begin: invalid start version after abort");
		}
		tx->nesting++;
		return true;
	}

	tx->start_version = get_clock();
	tx->end_version = tx->start_version;
	tx->active = true;
	tx->aborted = false;
	tx->read_only = true;
	TINYSTM_ASSERT(tx->start_version > 0, "begin: invalid start version");
	TINYSTM_ASSERT(tx->end_version >= tx->start_version, "begin: invalid validity range");

	return true;
}

inline void abort_tx()
{
	auto *tx = current_tx;
	TINYSTM_ASSERT(tx != nullptr, "abort_tx: tx is null");

	for (auto &w : tx->write_set) {
		word_t *addr = (word_t *)w.location.base_addr;
		*addr = w.old_word;
	}

	for (word_t *lock_addr : tx->locks_held) {
		Lock *lock = g_locks.get((word_t)lock_addr);
		TINYSTM_ASSERT(lock != nullptr, "abort_tx: lock is null");
		lock->inc_abort(tx->abort_count);
	}
	tx->locks_held.clear();

	tx->aborted = true;
	tx->abort_count++;
	TINYSTM_ASSERT(tx->abort_count >= 0 && tx->abort_count < 1000,
	               "abort_tx: excessive abort count");
}

inline bool commit()
{
	auto *tx = current_tx;
	TINYSTM_ASSERT(tx != nullptr, "commit: tx is null");

	if (!tx || !tx->active) {
		return false;
	}

	if (tx->nesting > 1) {
		tx->nesting--;
		return true;
	}

	TINYSTM_ASSERT(tx->start_version > 0, "commit: invalid start version");
	TINYSTM_ASSERT(tx->end_version >= tx->start_version,
	               "commit: invalid validity range");

	if (tx->aborted) {
		abort_tx();
		tx->active = false;
		return false;
	}

	word_t commit_version = 0;

	if (!tx->read_only && !tx->write_set.empty()) {
		TINYSTM_ASSERT(tx->write_set.size() > 0, "commit: write_set empty for update tx");
		commit_version = increment_clock();

		if (commit_version > tx->start_version + 1) {
			word_t tx_id = (word_t)tx;
			for (auto &r : tx->read_set) {
				Lock *lock = g_locks.get(r.location.base_addr);
				TINYSTM_ASSERT(lock != nullptr, "commit: lock is null");

				if (lock->is_locked_by(tx_id)) {
					continue;
				}

				word_t current_version = lock->get_version();
				word_t current_incarnation = lock->get_incarnation();

				if (current_version != r.observed_version ||
				    current_incarnation != r.observed_incarnation) {
					abort_tx();
					tx->active = false;
					return false;
				}
			}
		}

		for (auto &w : tx->write_set) {
			Lock *lock = g_locks.get(w.location.base_addr);
			lock->unlock(commit_version, 0);
		}
	}

	tx->write_set.clear();
	tx->read_set.clear();
	tx->locks_held.clear();

	tx->active = false;
	return true;
}

inline bool active() { return current_tx && current_tx->active; }

inline bool aborted() { return current_tx && current_tx->aborted; }

static word_t read_word_wt(Transaction *tx, volatile word_t *addr, ValueType type)
{
	if (!tx || !tx->active)
		return *addr;

	ByteOffset bo((word_t)addr);
	Lock *lock = g_locks.get(bo.base_addr);

	for (auto &w : tx->write_set) {
		if (same_location(w.location, bo)) {
			return w.new_word;
		}
	}

	if (lock->is_locked_by((word_t)tx)) {
		for (auto &w : tx->write_set) {
			if (same_location(w.location, bo)) {
				return w.new_word;
			}
		}
		return *addr;
	}

	word_t l = lock->get();

	while (true) {
		if (lock->is_locked()) {
			l = lock->get();
			continue;
		}

		word_t value = *addr;
		word_t l2 = lock->get();

		if (l != l2) {
			l = l2;
			continue;
		}

		word_t version = lock->get_version();
		word_t incarnation = lock->get_incarnation();

		if (version > tx->end_version) {
			if (tx->read_only) {
				tx->aborted = true;
				return value;
			}

			bool extended = false;
			for (auto &r : tx->read_set) {
				Lock *rl = g_locks.get(r.location.base_addr);
				word_t rv = rl->get_version();
				if (rv > tx->start_version) {
					extended = true;
					break;
				}
			}

			if (!extended) {
				tx->aborted = true;
				return value;
			}

			tx->end_version = get_clock();
			if (tx->end_version < version) {
				tx->aborted = true;
				return value;
			}
		}

		ReadLogEntry r;
		r.lock_addr = (word_t *)lock;
		r.location = bo;
		r.observed_version = version;
		r.observed_incarnation = incarnation;
		r.observed_word = value;
		r.type = type;
		tx->read_set.push_back(r);

		return value;
	}
}

static void write_word_wt(Transaction *tx,
                          volatile word_t *addr,
                          word_t value,
                          ValueType type)
{
	if (!tx || !tx->active) {
		*addr = value;
		return;
	}

	bool was_read_only = tx->read_only;
	tx->read_only = false;

	ByteOffset bo((word_t)addr);

	for (auto &w : tx->write_set) {
		if (same_location(w.location, bo)) {
			w.new_word = value;
			return;
		}
	}

	Lock *lock = g_locks.get(bo.base_addr);
	word_t l = lock->get();

	if (!lock->is_locked()) {
		word_t version = lock->get_version();
		word_t incarnation = lock->get_incarnation();

		WriteLogEntry w;
		w.location = bo;
		w.old_word = *addr;
		w.new_word = value;
		w.type = type;
		w.version = version;
		w.incarnation = incarnation;
		tx->write_set.push_back(w);

		word_t tx_id = (word_t)tx;
		if (lock->try_lock(tx_id, 0)) {
			*addr = value;
			tx->locks_held.push_back((word_t *)lock);
		} else if (lock->is_locked_by(tx_id)) {
			*addr = value;
		} else {
			tx->aborted = true;
		}
	} else if (lock->is_locked_by((word_t)tx)) {
		for (auto &w : tx->write_set) {
			if (same_location(w.location, bo)) {
				w.new_word = value;
				*addr = value;
				return;
			}
		}
	} else {
		tx->aborted = true;
	}
}

inline uint8_t tm_read_i1(volatile uint8_t *addr)
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return *addr;

	word_t word = read_word_wt(tx,
	                           (volatile word_t *)((word_t)addr & ~7),
	                           ValueType::UINT8);
	uint8_t off = (word_t)addr & 7;
	return (word >> (off * 8)) & 0xFF;
}

inline uint16_t tm_read_i2(volatile uint16_t *addr)
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return *addr;

	word_t word = read_word_wt(tx,
	                           (volatile word_t *)((word_t)addr & ~7),
	                           ValueType::UINT16);
	uint8_t off = (word_t)addr & 7;
	return (word >> (off * 8)) & 0xFFFF;
}

inline uint32_t tm_read_i4(volatile uint32_t *addr)
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return *addr;

	return (uint32_t)read_word_wt(tx, (volatile word_t *)addr, ValueType::UINT32);
}

inline uint64_t tm_read_i8(volatile uint64_t *addr)
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return *addr;

	return (uint64_t)read_word_wt(tx, (volatile word_t *)addr, ValueType::UINT64);
}

inline float tm_read_f4(volatile float *addr)
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return *addr;

	word_t bits = read_word_wt(tx, (volatile word_t *)addr, ValueType::FLOAT);
	return *(float *)&bits;
}

inline double tm_read_f8(volatile double *addr)
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return *addr;

	word_t bits = read_word_wt(tx, (volatile word_t *)addr, ValueType::DOUBLE);
	return *(double *)&bits;
}

inline void *tm_read_ptr(volatile void **addr)
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return (void *)*addr;

	return (void *)read_word_wt(tx, (volatile word_t *)addr, ValueType::POINTER);
}

inline void tm_write_i1(volatile uint8_t *addr, uint8_t val)
{
	auto *tx = current_tx;
	if (!tx || !tx->active) {
		*addr = val;
		return;
	}

	word_t *word_addr = (word_t *)((word_t)addr & ~7);
	word_t word = *word_addr;
	uint8_t off = (word_t)addr & 7;
	word_t mask = (0xFFULL << (off * 8));
	word = (word & ~mask) | ((word_t)val << (off * 8));
	write_word_wt(tx, (volatile word_t *)word_addr, word, ValueType::UINT8);
}

inline void tm_write_i2(volatile uint16_t *addr, uint16_t val)
{
	auto *tx = current_tx;
	if (!tx || !tx->active) {
		*addr = val;
		return;
	}

	word_t *word_addr = (word_t *)((word_t)addr & ~7);
	word_t word = *word_addr;
	uint8_t off = (word_t)addr & 7;
	word_t mask = (0xFFFFULL << (off * 8));
	word = (word & ~mask) | ((word_t)val << (off * 8));
	write_word_wt(tx, (volatile word_t *)word_addr, word, ValueType::UINT16);
}

inline void tm_write_i4(volatile uint32_t *addr, uint32_t val)
{
	auto *tx = current_tx;
	if (!tx || !tx->active) {
		*addr = val;
		return;
	}

	write_word_wt(tx, (volatile word_t *)addr, (word_t)val, ValueType::UINT32);
}

inline void tm_write_i8(volatile uint64_t *addr, uint64_t val)
{
	auto *tx = current_tx;
	if (!tx || !tx->active) {
		*addr = val;
		return;
	}

	write_word_wt(tx, (volatile word_t *)addr, (word_t)val, ValueType::UINT64);
}

inline void tm_write_f4(volatile float *addr, float val)
{
	auto *tx = current_tx;
	if (!tx || !tx->active) {
		*addr = val;
		return;
	}

	write_word_wt(tx, (volatile word_t *)addr, *(word_t *)&val, ValueType::FLOAT);
}

inline void tm_write_f8(volatile double *addr, double val)
{
	auto *tx = current_tx;
	if (!tx || !tx->active) {
		*addr = val;
		return;
	}

	write_word_wt(tx, (volatile word_t *)addr, *(word_t *)&val, ValueType::DOUBLE);
}

inline void tm_write_ptr(volatile void **addr, void *val)
{
	auto *tx = current_tx;
	if (!tx || !tx->active) {
		*addr = val;
		return;
	}

	write_word_wt(tx, (volatile word_t *)addr, (word_t)val, ValueType::POINTER);
}

} // namespace tinystm
