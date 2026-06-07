#pragma once

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "tm_common.hpp"
#include "tm_spin_token.hpp"
#include "tm_event_logger.hpp"
#include <random>
#include <thread>

// ── TinySTM assertion macro ────────────────────────────────
// Uses shared TM_ASSERT from tm_common.hpp

// Assertions: use TM_ASSERT / TM_ASSERT_VALID_TX from tm_common.hpp.

namespace tinystm
{

using word_t = uint64_t;

using stm::any_type_mapping;
using stm::any_type_t;
using stm::ByteOffset;
using stm::fill_any_type;
using stm::read_value_from_addr;
using stm::return_any_type;
using stm::type_size;
using stm::ValueType;
using stm::write_value_to_addr;

extern __thread sigjmp_buf *jmpbuf;

constexpr word_t OWNED_BITS = 2L;       // read mask is not used, but kept
constexpr word_t INCARNATION_BITS = 3L; // wt only
constexpr word_t LOCK_BITS = OWNED_BITS + INCARNATION_BITS;
constexpr word_t THREAD_BITS = 13L;
constexpr word_t MAX_THREADS = 1L << THREAD_BITS;
constexpr word_t META_BITS = LOCK_BITS + THREAD_BITS;

constexpr word_t WRITE_MASK = 0x01L;
constexpr word_t READ_MASK = 0x02L;                                   // not used
constexpr word_t OWNED_MASK = (WRITE_MASK | READ_MASK);               // lock
constexpr word_t INCARNATION_MASK = 0x7L;                             // wt only
constexpr word_t LOCK_MASK = (0x7L << INCARNATION_BITS) | OWNED_MASK; // wt only
constexpr word_t THREAD_MASK = MAX_THREADS - 1;

constexpr word_t VERSION_MASK = ((~OWNED_MASK) & (~(INCARNATION_MASK << OWNED_BITS)) &
                                 (~(THREAD_MASK << LOCK_BITS))) >>
                                META_BITS;
constexpr word_t VERSION_MAX = VERSION_MASK;

constexpr word_t LOCK_EXTRA_BITS = 3L; // Probably cacheline granularity is better
constexpr word_t EXTRA_BITS_MASK = 7L;

constexpr word_t LOCK_ARRAY_LOG_SIZE = 14L;
constexpr word_t LOCK_ARRAY_SIZE = 1L << LOCK_ARRAY_LOG_SIZE;
constexpr word_t LOCK_ARRAY_MASK = LOCK_ARRAY_SIZE - 1L;

static void setjmp(sigjmp_buf *buf) { jmpbuf = buf; }

class Lock
{
public:
	std::atomic<word_t> state{0};

	word_t get() const { return state.load(std::memory_order_acquire); }

	word_t get_owner() const { return (get() & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS; }

	void unlock(word_t tx_id)
	{
		// TINYSTM_ASSERT(is_locked() && get_owner() == tx_id, "Not the owner of the lock");
		if (is_locked() && get_owner() == tx_id) { // TODO: should not happen!
			state.fetch_and((~OWNED_MASK) & (~(THREAD_MASK << LOCK_BITS)),
			                std::memory_order_release); // sets owned and tx_id bits to 0
		}
	}

	void reset_version()
	{
		state.fetch_and(~(VERSION_MASK << META_BITS), std::memory_order_release);
	}

	void unlock_with_version(word_t tx_id, word_t v)
	{
		TM_ASSERT(get_owner() == tx_id, "Not the owner of the lock");
		state.store(((v & VERSION_MASK) << META_BITS), std::memory_order_release);
	}

	void unlock_with_version_and_incarnation(word_t tx_id,
	                                         word_t new_version,
	                                         word_t new_incarnation)
	{
		TM_ASSERT(get_owner() == tx_id, "Not the owner of the lock");
		word_t desired = ((new_version & VERSION_MASK) << META_BITS) |
		                 ((new_incarnation & INCARNATION_MASK) << OWNED_BITS);
		state.store(desired, std::memory_order_release);
	}

	bool try_lock(word_t tx_id)
	{
		word_t current_state = state.load(std::memory_order_acquire);
		if ((current_state & OWNED_MASK) != 0) {
			return false;
		}
		word_t expected = current_state & ~OWNED_MASK; // Lock must not be taken
		word_t desired = (current_state & (VERSION_MASK << META_BITS)) |
		                 (((tx_id & THREAD_MASK) << LOCK_BITS) | WRITE_MASK) |
		                 (current_state & (INCARNATION_MASK << OWNED_BITS));
		TM_ASSERT((desired & WRITE_MASK) == 1 &&
		                   ((desired & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS) == tx_id,
		               "Wrong lock configuration");
		if ((expected & OWNED_MASK) != 0) {
			return false;
		}
		TM_ASSERT(((expected & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS) == 0,
		               "Lock is unlocked with a owner");
		bool res = state.compare_exchange_strong(expected, desired);
		TM_ASSERT(!res || (res && state.load(std::memory_order_acquire) == desired),
		               "CAS did not work as expected");
		return res;
	}

	bool try_lock_with_incarnation(word_t tx_id, word_t incarnation)
	{
		word_t expected = 0;
		word_t desired = ((tx_id & THREAD_MASK) << LOCK_BITS) |
		                 ((incarnation & INCARNATION_MASK) << OWNED_BITS);
		return state.compare_exchange_strong(expected,
		                                     desired,
		                                     std::memory_order_acquire,
		                                     std::memory_order_acquire);
	}

	virtual bool is_locked_by(word_t tx_id) const
	{
		word_t s = state.load(std::memory_order_acquire);
		return ((s & OWNED_MASK) != 0) &&
		       (((s & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS) == tx_id);
	}

	bool is_locked() const
	{
		word_t s = state.load(std::memory_order_acquire);
		return (s & OWNED_MASK) != 0;
	}

	virtual word_t get_version() const
	{
		word_t s = state.load(std::memory_order_acquire);
		return (s & (VERSION_MASK << META_BITS)) >> META_BITS;
	}

	word_t get_incarnation() const
	{
		word_t s = state.load(std::memory_order_acquire);
		return (s >> OWNED_BITS) & INCARNATION_MASK;
	}

	void inc_abort(word_t current_incarnation) // also unlocks
	{
		word_t current_state = state.load(std::memory_order_acquire);
		word_t new_incarnation = ((current_state >> OWNED_BITS) & INCARNATION_MASK) + 1;
		// TODO: does this work with wrap around?
		word_t current_version = (current_state >> META_BITS);
		word_t desired = (current_version << META_BITS) |
		                 ((new_incarnation & INCARNATION_MASK) << OWNED_BITS);
		state.store(desired, std::memory_order_release);
	}
};

template <typename L> class LockTable
{
public:
	L locks[LOCK_ARRAY_SIZE];

	inline L &get(word_t addr)
	{
		return locks[(addr >> LOCK_EXTRA_BITS) & LOCK_ARRAY_MASK];
	}

	inline void reset_versions()
	{
		for (L &l : locks) {
			l.state.store(0, std::memory_order_release);
		}
	}
};

template <                 //
    typename ReadLogEntry, //
    typename WriteLogEntry //
    >                      //
class Transaction
{
public:
	volatile word_t id = 0;
    word_t start_version = 0;
    volatile word_t end_version = 0;
    word_t commit_version = 0; // set by commit() for EBR
	bool active = false;
	bool aborted = false;
	bool read_only = true;
	int abort_count = 0;
	bool is_retry = false;
	int nesting = 1;
	// ═══════════════════════════════════════════════════════════════
	//  CRITICAL: These containers use the STANDARD allocator.
	// ═══════════════════════════════════════════════════════════════
	// They are part of the runtime (compiled separately, NEVER fed
	// through the TM plugin).  Their internal bucket-array allocations
	// go through ::operator new → direct heap (no tm_malloc, no spec
	// tracking).  This is intentional: if they were spec-tracked, an
	// abort would free the bucket arrays while the unordered_map still
	// holds a pointer to them → use-after-free on the next begin().
	//
	// See tm_alloc_overrides.hpp for the full discussion.
	std::unordered_map<void *, ReadLogEntry> read_set;
	std::unordered_map<void *, WriteLogEntry> write_set;
	std::vector<Lock *> locks_held;

	void reset()
	{
		start_version = 0;
		end_version = 0;
		active = false;
		read_only = true;
		abort_count = 0;
		is_retry = false;
		clear();
	}

	void clear()
	{
		read_set.clear();
		write_set.clear();
		locks_held.clear();
	}

	void unlock_held_locks()
	{
		for (Lock *lock : locks_held) {
			lock->unlock(id);
		}
	}

	void unlock_held_locks_and_clear()
	{
		unlock_held_locks();
		clear();
	}
};

template <typename T,
          ValueType SZ,
          typename RL,
          typename WL,
          any_type_t (*read_word)(Transaction<RL, WL> *,
                                  void *,
                                  ValueType)>
inline T tm_read(            //
    Transaction<RL, WL> *tx, //
    T *addr                  //
)
{
	TM_ASSERT_VALID_TX(tx, "tinystm tm_read");

	any_type_t r = read_word(tx, (void *)addr, SZ);
	return return_any_type<T>(r);
}

template <typename T,
          ValueType SZ,
          typename RL,
          typename WL,
          void (*write_word)(Transaction<RL, WL> *,
                             void *,
                             any_type_t,
                             ValueType)>
inline void                  //
tm_write(                    //
    Transaction<RL, WL> *tx, //
    T *addr,                 //
    T val                    //
)
{
	TM_ASSERT_VALID_TX(tx, "tinystm tm_write");

	any_type_t w;
	fill_any_type(w, &val, SZ);
	write_word(tx, (void *)addr, w, SZ);
}

extern std::atomic<word_t> g_clock;
extern std::atomic<tinystm::word_t> thr_counter;
extern std::atomic<tinystm::word_t> reset_locks_thr;
extern std::atomic<uint64_t> g_tm_abort_count;
extern thread_local bool rng_initialized;
extern thread_local std::mt19937 rng;

// ── Random Exponential Backoff ────────────────────────────────
// Thread-local Mersenne Twister seeded once per thread.
constexpr int K_MAX_BACKOFF_DELAY_US = 100000;

inline void  //
init_rand()  //
{
	if (!rng_initialized) {
		std::random_device rd;
		rng.seed(rd());
		rng_initialized = true;
	}
}

// Exponential backoff using the thread-local RNG.  Callers pass their
// per-tx abort_count so the mean delay decreases as the TX retries.
inline void      //
random_backoff(  //
    unsigned abort_count)
{
	init_rand();
	std::exponential_distribution<> dist((double)1 / (double)(abort_count + 1));
	int delay = std::min(dist(rng), (double)K_MAX_BACKOFF_DELAY_US);
	std::this_thread::sleep_for(std::chrono::microseconds(delay));
}

inline void init()
{
    // Initialize the TM address-space region (mmap bump allocator).
    // If this fails, isTMAddress() returns false for all addresses,
    // so all TM reads/writes bypass the lock table (safe but dead).
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed — TM address space unavailable\n");
        std::abort();
    }
	g_clock.store(1, std::memory_order_release);
	thr_counter.store(1, std::memory_order_release);
	// Events already no-ops unless TM_EVENT_LOG defined.
}

inline void exit()
{
	stm::tm_region_destroy();
}

inline word_t get_clock()
{
	word_t res;
	while ((res = g_clock.load(std::memory_order_acquire)) >= VERSION_MASK)
		std::this_thread::sleep_for(
		    std::chrono::microseconds(1000)); // someone is reseting the clock
	return res;
}

static void reset_locks();

inline word_t increment_clock(word_t tx_id)
{
	word_t res = g_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (res >= VERSION_MAX) {
		word_t expect = 0L;
		word_t desired = tx_id;
		if (reset_locks_thr.compare_exchange_strong(expect, desired)) {
			reset_locks();
			g_clock.store(1, std::memory_order_release);
			reset_locks_thr.store(expect, std::memory_order_release);
			res = g_clock.load(std::memory_order_acquire);
		} else {
			while ((res = get_clock()) >= VERSION_MASK)
				std::this_thread::sleep_for(
				    std::chrono::microseconds(1000)); // someone is reseting the clock
		}
	}

	return res;
}

} // namespace tinystm
