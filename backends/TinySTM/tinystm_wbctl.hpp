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
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <thread>

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

	TINYSTM_ASSERT(tx, "tx not defined");
	if (tx->active)
		return true;

	tx->clear();
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
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
	if (tx->abort_count < 20 || tx->abort_count % 100 == 0) {
		fprintf(stderr, "[ABORT tx=%llu count=%d ws=%zu rs=%zu]\n",
		        (unsigned long long)tx->id, tx->abort_count,
		        tx->write_set.size(), tx->read_set.size());
		fflush(stderr);
	}
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

inline bool //
commit()    //
{
	auto *tx = current_tx_wbctl;
	volatile word_t commit_version = 0;

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	if (!tx->read_only) { // Acquire locks and write-back

		// Lock acquisition phase — collect addresses and sort for global lock ordering
		std::vector<void *> sorted_addrs;
		sorted_addrs.reserve(tx->write_set.size());
		for (auto &it : tx->write_set)
			sorted_addrs.push_back(it.first);
		std::sort(sorted_addrs.begin(), sorted_addrs.end(), compareByAddr);
		for (void *addr : sorted_addrs) {
			auto &w = tx->write_set[addr];
			ByteOffset bo((word_t)addr);
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
		commit_version = increment_clock(tx->id);
		if (commit_version < tx->end_version)
			abort_tx(); // version overflow

		// Check if there were transactions in between
		if (commit_version != tx->end_version + 1) {
			if (!extend()) {
				abort_tx(); // can leave gaps in the global clock
			}
		}

		// increment_clock(tx->id); // Reading above and incrementing here does not work

		// Write-back phase
		for (auto &it : tx->write_set) {
			auto &addr = it.first;
			auto &w = it.second;
			ByteOffset bo((word_t)addr);
			Lock *lock = &g_locks_wbctl.get(bo.base_addr);
			write_value_to_addr(addr, w.new_val, w.type);
		}

		for (auto &it : tx->write_set) {
			auto &addr = it.first;
			auto &w = it.second;
			ByteOffset bo((word_t)addr);
			Lock *lock = &g_locks_wbctl.get(bo.base_addr);
			if (!lock->is_locked_by(tx->id)) {
				// Already unlocked by a previous write entry for the same
				// 8-byte lock word (e.g., two adjacent 4-byte ints).  This is
				// valid — all entries have already been written back.
				continue;
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
	std::atomic_signal_fence(std::memory_order_seq_cst);
	ByteOffset bo((word_t)addr);

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

#ifdef DEBUG_WBCTL
	// Debug: detect corrupted addresses (32-bit truncation)
	static std::atomic<uint64_t> read_count{0};
	uint64_t rc = read_count++;
	uint64_t addr_bits = (uint64_t)addr;

	if ((addr_bits >> 48) != 0) {
		fprintf(stderr,
		        "[R%llu] addr=%p sz=%d tx=%llu\n",
		        rc,
		        (void *)addr_bits,
		        (int)sz,
		        (unsigned long long)tx->id);
		fflush(stderr);
	}
#endif

	// Corrupted address detection removed (was shadowing root cause).

	// Check write-set for this exact address
	auto w = tx->write_set.find(addr);
	if (w != tx->write_set.end()) {
		if (w->second.type == sz)
			return w->second.new_val;
		// POINTER and UINT64 are both 8 bytes and share storage in any_type_t
		// (MAP_ANY maps ptr and u8 to the same member).  LLVM type mapping can
		// write a value as UINT64 (ptrtoint in deque internals) and read it back
		// as POINTER, or vice versa.  Treat them as interchangeable.
		if ((sz == ValueType::POINTER && w->second.type == ValueType::UINT64) ||
		    (sz == ValueType::UINT64 && w->second.type == ValueType::POINTER)) {
			return w->second.new_val;
		}
		// REVERSE-type check: existing is narrower (UINT8/16/32), reading wider
		// (UINT64, POINTER).  Try to reconstruct the wider value from sub-word
		// write-set entries.  If all byte addresses have UINT8 entries, merge
		// them into the wider value.  This handles two cases:
		//   (a) memmove/memcpy writes the key of a pair byte-by-byte (via
		//       tm_write_i1), and then the same TX reads the key as UINT64
		//       during binary search.
		//   (b) deque map reallocation copies block pointers via memcpy
		//       (instrumented as UINT8 bytes), and the TX reads map entries
		//       as POINTER — without this merge, POINTER reads fall through
		//       to memory, get stale data (0), and pointer+offset produces
		//       tiny invalid addresses.
		if ((sz == ValueType::UINT64 || sz == ValueType::POINTER) &&
		    (w->second.type == ValueType::UINT8 || w->second.type == ValueType::UINT16 ||
		     w->second.type == ValueType::UINT32)) {
			uint64_t merged = 0;
			bool all_found = true;
			for (unsigned i = 0; i < 8; i++) {
				void *byte_addr = reinterpret_cast<void *>((uintptr_t)addr + i);
				auto it = tx->write_set.find(byte_addr);
				if (it != tx->write_set.end() && it->second.type == ValueType::UINT8) {
					merged |= (static_cast<uint64_t>(it->second.new_val.u1)) << (i * 8);
				} else {
					all_found = false;
					break;
				}
			}
			if (all_found) {
				any_type_t result;
				result.u8 = merged;
				return result;
			}
			goto read_from_memory;
		}
		// Wider-to-narrower: a wider write at this exact address covers
		// narrower sub-word reads (e.g. UINT32 field then UINT8 byte read
		// via memmove byte loop in deque internal operations).
		if (sz == ValueType::UINT8) {
			any_type_t result;
			if (w->second.type == ValueType::UINT64) {
				result.u1 = static_cast<uint8_t>(w->second.new_val.u8 & 0xFF);
				return result;
			}
			if (w->second.type == ValueType::UINT32) {
				result.u1 = static_cast<uint8_t>(w->second.new_val.u4 & 0xFF);
				return result;
			}
			if (w->second.type == ValueType::UINT16) {
				result.u1 = static_cast<uint8_t>(w->second.new_val.u2 & 0xFF);
				return result;
			}
		}
		if (sz == ValueType::UINT16 && w->second.type == ValueType::UINT64) {
			any_type_t result;
			result.u2 = static_cast<uint16_t>(w->second.new_val.u8 & 0xFFFF);
			return result;
		}
		if (sz == ValueType::UINT32 && w->second.type == ValueType::UINT64) {
			any_type_t result;
			result.u4 = static_cast<uint32_t>(w->second.new_val.u8 & 0xFFFFFFFF);
			return result;
		}
	}

	// GENERAL FALLBACK: reconstruct any read value from byte-level (UINT8)
	// write-set entries.  The memcpy/memmove byte-loop instrumentation
	// (instrumentMemoryIntrinsic) creates UINT8 entries at individual byte
	// addresses.  When a subsequent read expects a wider type (POINTER,
	// UINT64, UINT32, UINT16, DOUBLE) at the same address, we need to
	// merge the UINT8 entries rather than falling through to memory.
	{
		unsigned read_size = 0;
		switch (sz) {
		case ValueType::UINT8:   read_size = 1; break;
		case ValueType::UINT16:  read_size = 2; break;
		case ValueType::UINT32:
		case ValueType::FLOAT:   read_size = 4; break;
		case ValueType::UINT64:
		case ValueType::DOUBLE:
		case ValueType::POINTER: read_size = 8; break;
		default: break;
		}
		if (read_size > 0) {
			uint64_t merged = 0;
			bool all_byte = true;
			for (unsigned i = 0; i < read_size; i++) {
				void *byte_addr = reinterpret_cast<void *>((uintptr_t)addr + i);
				auto it = tx->write_set.find(byte_addr);
				if (it != tx->write_set.end() && it->second.type == ValueType::UINT8) {
					merged |= (static_cast<uint64_t>(it->second.new_val.u1)) << (i * 8);
				} else {
					all_byte = false;
					break;
				}
			}
			if (all_byte) {
				any_type_t result;
				result.u8 = merged;
				return result;
			}
		}
	}

	// For unaligned or non-matching reads: check if a wider write at a nearby
	// aligned address covers this byte.  A memmove byte loop (from
	// instrumentMemoryIntrinsic) reads bytes at offsets within a UINT32/UINT64
	// field — check UINT64 at the 8-byte aligned base, UINT32 at the 4-byte
	// aligned base, and UINT16 at the 2-byte aligned base.
	// IMPORTANT: Do NOT use addr != base_addr guards — a UINT32 at the same
	// 8-byte-aligned address as UINT64 must be checked independently.
	if (bo.offset != 0) {
		void *base_addr = reinterpret_cast<void *>(bo.base_addr);
		unsigned shift = static_cast<unsigned>(bo.offset) * 8;

		// Check UINT64 at 8-byte aligned (existing logic)
		auto w2 = tx->write_set.find(base_addr);
		if (w2 != tx->write_set.end() && w2->second.type == ValueType::UINT64) {
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

		// Check UINT32 at 4-byte aligned address (may equal base_addr for
		// offsets 0-3 — check independently of UINT64 above)
		void *u32_addr = reinterpret_cast<void *>((uintptr_t)addr & ~3ULL);
		auto w32 = tx->write_set.find(u32_addr);
		if (w32 != tx->write_set.end() && w32->second.type == ValueType::UINT32) {
			unsigned byte_off = static_cast<unsigned>((uintptr_t)addr - (uintptr_t)u32_addr);
			unsigned u32_shift = byte_off * 8;
			switch (sz) {
			case ValueType::UINT8: {
				any_type_t result;
				result.u1 = static_cast<uint8_t>(w32->second.new_val.u4 >> u32_shift);
				return result;
			}
			case ValueType::UINT16: {
				if (byte_off <= 2) {
					any_type_t result;
					result.u2 = static_cast<uint16_t>(w32->second.new_val.u4 >> u32_shift);
					return result;
				}
				break;
			}
			default:
				break;
			}
		}

		// Check UINT16 at 2-byte aligned address (may equal base_addr or
		// u32_addr — check independently)
		void *u16_addr = reinterpret_cast<void *>((uintptr_t)addr & ~1ULL);
		auto w16 = tx->write_set.find(u16_addr);
		if (w16 != tx->write_set.end() && w16->second.type == ValueType::UINT16) {
			unsigned byte_off = static_cast<unsigned>((uintptr_t)addr - (uintptr_t)u16_addr);
			if (byte_off < 2 && sz == ValueType::UINT8) {
				any_type_t result;
				result.u1 = static_cast<uint8_t>(w16->second.new_val.u2 >> (byte_off * 8));
				return result;
			}
		}
	}

read_from_memory:
	Lock *lock = &g_locks_wbctl.get(bo.base_addr);
	TINYSTM_ASSERT(!lock->is_locked_by(tx->id), "wbctl locks at commit time");
	volatile word_t l = lock->get();

	// NOTE: Every read goes through the full double-check protocol below.
	// DO NOT add a read_set cache shortcut here — doing so skips the second
	// lock read (double-check) and allows observing values written after the
	// transaction's snapshot point, violating opacity (see docs/proofs.md §4.1).

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
		tx->read_set.insert(std::pair(addr, r));

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

	// Discard writes to invalid addresses (e.g., nullptr or stack offsets
	// that escape via integer arithmetic).  This can happen when the TX
	// function evaluates a pointer-to-integer expression for a local
	// variable — the resulting integer looks like a tiny address.
	if (addr == nullptr || (uint64_t)addr < 0x100000) {
		return;
	}

	tx->read_only = false;

#ifdef DEBUG_WBCTL
	static std::atomic<uint64_t> allw{0};
	uint64_t aw = allw++;
	if (aw < 200 || (aw % 5000 == 0)) {
		fprintf(stderr,
		        "[W_ALL#%llu] addr=%p sz=%d val=0x%llx ws=%zu\n",
		        (unsigned long long)aw,
		        addr,
		        (int)sz,
		        (unsigned long long)val.u8,
		        tx->write_set.size());
		fflush(stderr);
	}
#endif

	// Found write-set entry at exact addr with matching type → update in place.
	{
		auto w = tx->write_set.find(addr);
		if (w != tx->write_set.end()) {
			if (w->second.type == sz) {
				w->second.new_val = val;
				return;
			}
			// Type mismatch at same addr.  If existing entry is UINT64 and this
			// write is a sub-word type, merge the byte(s) into the wider entry.
			if (w->second.type == ValueType::UINT64 && sz == ValueType::UINT8) {
				uint64_t merged = (w->second.new_val.u8 & ~(uint64_t)0xFF);
				merged |= (uint64_t)(val.u1);
				w->second.new_val.u8 = merged;
				return;
			}
		}
	}

	// For sub-word writes at an offset within an 8-byte word: if the aligned
	// address already has a UINT64 write-set entry, merge this write into it
	// rather than creating a separate entry that could mask the wider value.
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

	// If this write is wider than one byte, remove any overlapping sub-word
	// entries to prevent write-back order conflicts (e.g., memcpy byte entries
	// coexisting with a later UINT64 value write).
	{
		unsigned nbytes = 0;
		switch (sz) {
		case ValueType::UINT16:
			nbytes = 2;
			break;
		case ValueType::UINT32:
			nbytes = 4;
			break;
		case ValueType::UINT64:
			nbytes = 8;
			break;
		default:
			break;
		}
		if (nbytes > 1) {
			for (unsigned i = 0; i < nbytes; i++) {
				void *sub_addr = reinterpret_cast<void *>((uintptr_t)addr + i);
				auto it = tx->write_set.find(sub_addr);
				if (it != tx->write_set.end() && it->second.type != sz)
					tx->write_set.erase(it);
			}
			// Re-check for existing entry of matching type after cleanup
			auto existing = tx->write_set.find(addr);
			if (existing != tx->write_set.end()) {
				if (existing->second.type == sz) {
					existing->second.new_val = val;
					return;
				}
			}
		}
	}

	while (true) {
		Lock *lock = &g_locks_wbctl.get(bo.base_addr);
		volatile word_t l = lock->get();
		word_t owner = (l & (THREAD_MASK << LOCK_BITS)) >> LOCK_BITS;
		bool is_locked = (l & OWNED_MASK) != 0;

		TINYSTM_ASSERT(owner != tx->id, "WBCTL only locks at commit time");

		if (is_locked && !validate()) {
			abort_tx(); // returns to begin
		}

		if (is_locked)
			continue; // spin until the lock is released

		// Double-check: re-read lock to catch a concurrent writer that
		// acquired the lock between our first read and this re-read.
		volatile word_t l2 = lock->get();
		if (l != l2) {
			l = l2;
			continue;
		}

		word_t version = (l2 & (VERSION_MASK << META_BITS)) >> META_BITS;

		// Version-extension: if the observed version is newer than our
		// snapshot, extend the read-set first.  This matches the read path
		// protocol (read_word_ctl lines 411-417) and ensures opacity.
		if (version > tx->end_version) {
			if (extend()) {
				continue;
			} else {
				abort_tx();
			}
		}

		WriteLogEntry_wbctl w; // Create a new entry in writeset
		w.new_val = val;       // new val to write-back on commit
		w.type = sz;
		w.addr = addr;
		w.version = version;
		tx->write_set.insert(std::pair(addr, w));

		// Also add to read-set so that validate() catches version changes
		// from concurrent writers.  Without this, write-set-only addresses
		// are never validated during commit, allowing lost updates.
		ReadLogEntry_wbctl r;
		r.addr = addr;
		r.observed_version = version;
		r.observed_val = val;
		r.type = sz;
		tx->read_set.insert(std::pair(addr, r));

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
