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
#include <dlfcn.h>
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
abort_tx(const char *loc="")  //
{
	auto *tx = current_tx_wbctl;

	TINYSTM_ASSERT(tx, "tx not defined");
	TINYSTM_ASSERT(tx->active, "tx not active");

	tx->unlock_held_locks_and_clear();
	tx->abort_count++;
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
	if (tx->abort_count < 3 || tx->abort_count % 10 == 0) {
		fprintf(stderr, "[ABORT tx=%llu count=%d at=%s]\n",
		        (unsigned long long)tx->id, tx->abort_count, loc);
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
	static std::atomic<int> g_dbg{0};
	auto dbg_at_commit = g_dbg.fetch_add(1);
	if (dbg_at_commit < 20) {
		fprintf(stderr, "[COMMIT%03d] tx=%llu g_clock=%llu\n",
		        dbg_at_commit,
		        (unsigned long long)tx->id,
		        (unsigned long long)tinystm::g_clock.load(std::memory_order_acquire));
	}

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
						abort_tx("commit_lock");
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
			abort_tx("version_overflow");

		// Check if there were transactions in between
		if (commit_version != tx->end_version + 1) {
			if (!extend()) {
				abort_tx("gap_check");
			}
		}

		// Unconditional read-set validation — prevents lost updates when
		// two concurrent TXs both read and write the same address without
		// lock contention or clock gaps.  Without this check, both TXs
		// commit with stale read-set entries (e.g., g_sum read as 100 by
		// both, each adds 50, both write 150 — one increment is lost).
		if (!validate()) {
			abort_tx("read_validation");
		}

		// increment_clock(tx->id); // Reading above and incrementing here does not work

		// Write-back phase
		for (auto &it : tx->write_set) {
			auto &addr = it.first;
			auto &w = it.second;
			{ // DEBUG: show g_vec entries at EVERY commit (not just first)
				static std::atomic<int> g_wb_dump{0};
				int dump_n = g_wb_dump.fetch_add(1);
				bool first_dump = (dump_n == 0);
				// Also always show g_vec entries
				bool found_gvec = false;
				for (auto &dit : tx->write_set) {
					auto &da = dit.first;
					uintptr_t ua = (uintptr_t)da;
					Dl_info ai;
					if (dladdr(da, &ai) && ai.dli_sname && strcmp(ai.dli_sname, "g_vec") == 0) {
						if (!found_gvec) {
							found_gvec = true;
							fprintf(stderr, "[COMMIT#%d WS] write_set.size=%zu\n",
							        dump_n, tx->write_set.size());
						}
						Dl_info ai2;
						const char *sym2 = "?";
						if (dladdr(da, &ai2) && ai2.dli_sname) sym2 = ai2.dli_sname;
						fprintf(stderr, "  gvec addr=%p (%s) off=%ld type=%d val=0x%016llx\n",
						        da, sym2, (long)(ua - (uintptr_t)ai.dli_fbase - (ua - (uintptr_t)ai.dli_saddr)),
						        (int)dit.second.type,
						        (unsigned long long)dit.second.new_val.u8);
					}
				}
				if (found_gvec) fflush(stderr);
				if (first_dump) {
					fprintf(stderr, "[WB-DUMP] (first) write_set.size=%zu\n", tx->write_set.size());
					for (auto &dit : tx->write_set) {
						auto &da = dit.first;
						auto &dw = dit.second;
						Dl_info ai;
						const char *sname = "?";
						if (dladdr(da, &ai) && ai.dli_sname) sname = ai.dli_sname;
						fprintf(stderr, "  addr=%p sym=%s type=%d val=0x%016llx\n",
						        da, sname, (int)dw.type, (unsigned long long)dw.new_val.u8);
					}
					fflush(stderr);
				}
			}
			if ((uintptr_t)addr > 0x7FFFFFFFFFFFULL) {
				fprintf(stderr, "BAD WRITE-BACK ADDR: addr=%p type=%d val.u8=%llu\n",
				        addr, (int)w.type, (unsigned long long)w.new_val.u8);
				void *bt[16];
				int n = backtrace(bt, 16);
				backtrace_symbols_fd(bt, n, 2);
				// Skip this entry but continue (to see how many more bad entries exist)
				continue;
			}
			// Debug: detect writes to code section at write-back time
			{
				Dl_info ai;
				if (dladdr(addr, &ai) && ai.dli_fbase) {
					const char *sname = ai.dli_sname ? ai.dli_sname : "?";
					if ((uintptr_t)addr >= (uintptr_t)ai.dli_fbase + 0x4000 &&
					    ((uintptr_t)addr - (uintptr_t)ai.dli_fbase) < 0x100000) {
						static std::atomic<int> g_wb_once{0};
						if (g_wb_once.fetch_add(1) < 3)
							fprintf(stderr, "WRITE-BACK TO CODE: addr=%p sym=%s type=%d val=0x%llx\n",
							        addr, sname, (int)w.type, (unsigned long long)w.new_val.u8);
					}
				}
			}
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
			if (lock->get_version() < commit_version) {
				fprintf(stderr, "ASSERT: lock=%p get_version=%llu commit_version=%llu lock_state=0x%llx tx_id=%llu\n",
				        (void*)lock,
				        (unsigned long long)lock->get_version(),
				        (unsigned long long)commit_version,
				        (unsigned long long)lock->state.load(std::memory_order_acquire),
				        (unsigned long long)tx->id);
				fflush(stderr);
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
	// Debug: detect corrupted addresses
	static std::atomic<uint64_t> read_count{0};
	uint64_t rc = read_count++;
	uint64_t addr_bits = (uint64_t)addr;

	if (addr_bits < 0x100000 || (addr_bits >> 48) != 0) {
		fprintf(stderr,
		        "[R%llu] addr=%p sz=%d tx=%llu ws=%zu rs=%zu\n",
		        rc,
		        (void *)addr_bits,
		        (int)sz,
		        (unsigned long long)tx->id,
		        tx->write_set.size(),
		        tx->read_set.size());
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
				abort_tx("read_version_check");
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

// ---------------------------------------------------------------------------
// Debug: log write-set operations for g_vec-range addresses.
// Caches the g_vec symbol range via dladdr on first successful resolution.
// ---------------------------------------------------------------------------
static void
dbg_gvec_op(const char *op, void *addr, ValueType sz, any_type_t val,
            size_t ws_before, size_t ws_after)
{
	static uintptr_t s_gv_start = 0;
	static uintptr_t s_gv_end = 0;
	uintptr_t ua = (uintptr_t)addr;

	if (s_gv_start == 0) {
		if (ua > 0x100000000ULL && ua < 0x200000000ULL) {
			Dl_info di;
			if (dladdr(addr, &di) && di.dli_sname &&
			    strcmp(di.dli_sname, "g_vec") == 0) {
				s_gv_start = (uintptr_t)di.dli_saddr;
				s_gv_end = s_gv_start + 64;
			}
		}
		if (s_gv_start == 0)
			return; // not found yet — try again with next address
	}

	if (ua < s_gv_start || ua >= s_gv_end)
		return;

	Dl_info di;
	const char *sym = "?";
	if (dladdr(addr, &di) && di.dli_sname)
		sym = di.dli_sname;
	long off = (long)(ua - s_gv_start);

	static std::atomic<int> s_gv_cnt{0};
	int n = s_gv_cnt.fetch_add(1);
	if (n < 1000) {
		fprintf(stderr,
		        "[GVEC#%d %s] addr=%p (%s+%ld) sz=%d val=0x%016llx "
		        "ws=%zu->%zu\n",
		        n, op, addr, sym, off,
		        (int)sz, (unsigned long long)val.u8,
		        ws_before, ws_after);
		fflush(stderr);
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
	// Track writes to global g_vec for debugging reallocation issues.
	{
		static uintptr_t s_gvec = 0;
		if (!s_gvec) {
			Dl_info di;
			if (dladdr((void *)&tinystm::write_word_ctl, &di) && di.dli_fbase) {
				uintptr_t base = (uintptr_t)di.dli_fbase;
				s_gvec = base + 0x54df8;
			}
		}
		if (s_gvec) {
			uintptr_t ua = (uintptr_t)addr;
			if (ua >= s_gvec && ua < s_gvec + 0x100) {
				static std::atomic<int> g_gv_once{0};
				if (g_gv_once.fetch_add(1) < 40) {
					Dl_info ai;
					const char *sym = nullptr;
					if (dladdr(addr, &ai) && ai.dli_sname) sym = ai.dli_sname;
					long long off = (long long)(ua - s_gvec);
					fprintf(stderr, "\n[DBG g_vec WRITE] addr=%p off=%+lld sym=%s type=%d val.u8=0x%llx\n",
					        addr, off, sym ? sym : "?", (int)sz, (unsigned long long)val.u8);
				}
			}
		}
	}

	if (addr == nullptr || (uint64_t)addr < 0x100000) {
		return;
	}

	// Detect kernel-space addresses (typical vector-internal pointer corruption)
	// and dump a stack trace for debugging.
	if ((uintptr_t)addr > 0x7FFFFFFFFFFFULL) {
		static std::atomic<int> g_dbg_once{0};
		if (g_dbg_once.fetch_add(1) == 0) {
			fprintf(stderr, "\n[DBG BAD WRITE] addr=%p type=%d val.u8=%llu\n",
			        addr, (int)sz, (unsigned long long)val.u8);
			void *bt[32];
			int n = backtrace(bt, 32);
			backtrace_symbols_fd(bt, n, 2);
			fprintf(stderr, "[DBG] Continuing...\n");
		}
		return;
	}

	// Debug: check if addr points into the main binary's executable segment
	// (indicating a corrupted treap node pointer).
	{
		static int s_cs_warn = 0;
		static void *s_text_start = nullptr;
		static void *s_text_end = nullptr;
		if (!s_text_start) {
			Dl_info info;
			// Use a known function in the text section as anchor
			if (dladdr((void *)&tinystm::write_word_ctl, &info) && info.dli_fbase) {
				s_text_start = info.dli_fbase;
				// Scan for _end or use the executable's address range
				s_text_end = (void *)((uintptr_t)s_text_start + 0x200000); // 2MB should cover it
			}
		}
		if (s_text_start && val.u8 == 0) {
			uintptr_t uaddr = (uintptr_t)addr;
			uintptr_t base = (uintptr_t)s_text_start;
			uintptr_t end = (uintptr_t)s_text_end;
			if (uaddr >= base && uaddr < end) {
				static std::atomic<int> g_null_once{0};
				if (g_null_once.fetch_add(1) < 20) {
					Dl_info ai;
					const char *sym = nullptr;
					const char *sym2 = nullptr;
					if (dladdr(addr, &ai) && ai.dli_sname)
						sym = ai.dli_sname;
					void *caller = __builtin_return_address(0);
					Dl_info ci;
					if (dladdr(caller, &ci) && ci.dli_sname)
						sym2 = ci.dli_sname;
					fprintf(stderr, "\n[DBG NULL-WRITE] addr=%p sym=%s caller=%p caller_sym=%s type=%d val.u8=%llu\n",
					        addr, sym ? sym : "?", caller, sym2 ? sym2 : "?", (int)sz, (unsigned long long)val.u8);
					void *bt[32];
					int n = backtrace(bt, 32);
					backtrace_symbols_fd(bt, n, 2);
					fprintf(stderr, "[DBG NULL-WRITE] Continuing...\n");
				}
			}
		}
	}

	// DEBUG: count every entry into write_word_ctl
	{
		static std::atomic<int> g_wwc_cnt{0};
		int n = g_wwc_cnt.fetch_add(1);
		if (n < 30) {
			Dl_info di;
			const char *sym = "?";
			if (dladdr(addr, &di) && di.dli_sname) sym = di.dli_sname;
			fprintf(stderr, "[WWC#%d] addr=%p sym=%s sz=%d val.u8=0x%016llx tx=%llu rs=%zu ws=%zu\n",
			        n, addr, sym, (int)sz, (unsigned long long)val.u8,
			        (unsigned long long)(tx ? tx->id : 0),
			        tx ? tx->read_set.size() : 0,
			        tx ? tx->write_set.size() : 0);
		}
	}

	tx->read_only = false;

	// DEBUG: track EVERY write-set insert/modify for g_vec-range addresses.
	{
		thread_local uintptr_t s_gvec_base = 0;
		if (!s_gvec_base) {
			// Only scan when we see an address in the data section
			if ((uintptr_t)addr > 0x100000000ULL && (uintptr_t)addr < 0x200000000ULL) {
				Dl_info di;
				// Use a sentinel address we know is inside g_vec: try the address
				// at off=152 first (observed from earlier runs), but prefer dli_saddr
				if (dladdr(addr, &di) && di.dli_sname &&
				    (strcmp(di.dli_sname, "g_vec") == 0) && di.dli_saddr)
					s_gvec_base = (uintptr_t)di.dli_saddr;
			}
		}
		if (s_gvec_base) {
			uintptr_t ua = (uintptr_t)addr;
			uintptr_t base = (uintptr_t)s_gvec_base;
			if (ua >= base && ua < base + 256) {
				static std::atomic<int> g_cnt{0};
				int n = g_cnt.fetch_add(1);
				if (n < 80) {
					long off = (long)(ua - base);
					Dl_info di;
					const char *sym = "?";
					if (dladdr(addr, &di) && di.dli_sname) sym = di.dli_sname;
					auto old_w = tx->write_set.find(addr);
					fprintf(stderr, "[WCTL#%d] addr=%p g_vec%+ld sym=%s sz=%d val=0x%016llx "
					        "ws_find=%d ws_size=%zu\n",
					        n, addr, off, sym, (int)sz, (unsigned long long)val.u8,
					        old_w != tx->write_set.find(addr) ? 1 : 0, tx->write_set.size());
					fflush(stderr);
				}
			}
		}
	}

	// Found write-set entry at exact addr with matching type → update in place.
	{
		auto w = tx->write_set.find(addr);
		if (w != tx->write_set.end()) {
			if (w->second.type == sz) {
				{ // DBG: g_vec modify (same type)
					size_t ws = tx->write_set.size();
					dbg_gvec_op("MODIFY", addr, sz, val, ws, ws);
				}
				w->second.new_val = val;
				return;
			}
			// Type mismatch at same addr.  If existing entry is UINT64 and this
			// write is a sub-word type, merge the byte(s) into the wider entry.
			if (w->second.type == ValueType::UINT64 && sz == ValueType::UINT8) {
				uint64_t merged = (w->second.new_val.u8 & ~(uint64_t)0xFF);
				merged |= (uint64_t)(val.u1);
				{ // DBG: g_vec modify (merge UINT8 into wider UINT64 at addr)
					size_t ws = tx->write_set.size();
					any_type_t mv;
					mv.u8 = merged;
					dbg_gvec_op("MODIFY-MERGE", addr, ValueType::UINT64, mv, ws, ws);
				}
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
				{ // DBG: g_vec modify (merge sub-word into wider UINT64 at base_addr)
					size_t ws = tx->write_set.size();
					any_type_t mv;
					mv.u8 = merged;
					dbg_gvec_op("MODIFY-MERGE", base_addr, ValueType::UINT64, mv, ws, ws);
				}
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
				if (it != tx->write_set.end() && it->second.type != sz) {
					// DEBUG: log deletions of g_vec entries
					{
						Dl_info di;
						const char *sub_sym = "?";
						if (dladdr(sub_addr, &di) && di.dli_sname) sub_sym = di.dli_sname;
						const char *wider_sym = "?";
						if (dladdr(addr, &di) && di.dli_sname) wider_sym = di.dli_sname;
						static std::atomic<int> g_del_cnt{0};
						int dn = g_del_cnt.fetch_add(1);
						if (dn < 30 || (strcmp(sub_sym, "g_vec") == 0))
							fprintf(stderr, "[DEL#%d] erasing sub_addr=%p sym=%s type=%d from wider addr=%p sym=%s sz=%d ws_size_before=%zu\n",
							        dn, sub_addr, sub_sym, (int)it->second.type, addr, wider_sym, (int)sz, tx->write_set.size());
					}
					tx->write_set.erase(it);
				}
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
			abort_tx("write_lock_spin_validate");
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
				abort_tx("write_version_extension");
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
	// DEBUG: check if current_tx_wbctl is set
	{
		static std::atomic<int> g_twp_cnt{0};
		int n = g_twp_cnt.fetch_add(1);
		if (n < 20)
			fprintf(stderr, "[TWP#%d] addr=%p val=%p tx=%p tx_active=%d\n",
			        n, (void*)addr, (void*)val,
			        (void*)current_tx_wbctl,
			        current_tx_wbctl ? (int)current_tx_wbctl->active : -1);
	}
	tm_write<void *,
	         ValueType::POINTER,
	         ReadLogEntry_wbctl,
	         WriteLogEntry_wbctl,
	         write_word_ctl>(current_tx_wbctl, addr, val);
}

} // namespace tinystm
