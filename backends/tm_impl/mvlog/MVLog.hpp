#pragma once

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include "tm_bloom_filter.hpp"
#include "tm_common.hpp"
#include "tm_event_logger.hpp"
#include "tm_platform.hpp"

// MVLog — Multi-Version Log-based STM.
//
// Every transaction negotiates its commit position (a log slot) with one
// atomic fetch_add on g_next at tm_begin().  Commit is a pure append to a
// global, ordered commit log: the write-set is stored once, then made visible
// with a release store.  Readers never block and never take a lock:
//
//   - FAST PATH: a global no-false-negative Bloom filter (g_dirty) of
//     addresses written by committed-but-unreclaimed entries.  A clean miss
//     means g_mem (the TM region) holds the newest committed value for the
//     reader's snapshot, so the read is a plain load + one filter test.
//   - SLOW PATH: resolve through g_index (addr -> newest committed writer
//     slot) and snoop the writer's immutable write-set in the log.
//
// Commit waits for every predecessor slot in the live window [g_wm, slot) to
// resolve (enforcing commit order == slot-claim order, which is what keeps
// g_index monotonic and lets value-validation see a frozen commit state),
// then value-validates the read-set against the final index, then publishes.
// Read-only transactions take the same wait+validate path (they must, to
// prevent read-skew: a predecessor can commit at any time before this
// transaction's slot resolves, and only waiting gives a frozen snapshot).
//
// Reclamation: when the live window exceeds a threshold, the publisher folds
// the folded-out prefix [g_wm, slot) back into g_mem (in slot order), advances
// the watermark, then clears g_dirty.  Log entries form a ring (kLogSlots)
// indexed by slot & kLogMask; a per-entry slot tag detects recycling.
//
// The protocol is verified against the TLA+ model in docs/proofs/MVLog.tla
// (821,655 states generated / 329,041 distinct, 0 errors).  See
// backends/tm_impl/mvlog/Implementation_notes.md for the full design.
#ifndef NDEBUG
#define MVLOG_ASSERT_VALID_TX(tx, msg)                                                 \
	TM_ASSERT((tx) != nullptr, msg);                                                   \
	TM_ASSERT((tx)->active, "Transaction must be active: " msg);                       \
	TM_ASSERT(!(tx)->aborted, "Transaction must not be aborted: " msg)
#else
#define MVLOG_ASSERT_VALID_TX(tx, msg) /* EMPTY */
#endif

namespace mvlog
{

constexpr const char *VERSION = "0.1.0";

using word_t = uint64_t;
using stm::any_type_mapping;
using stm::any_type_t;
using stm::fill_any_type;
using stm::read_value_from_addr;
using stm::return_any_type;
using stm::ValueType;
using stm::write_value_to_addr;

// ── Global geometry ────────────────────────────────────────────────
// Ring-log size and reclamation threshold.  The live window is bounded by
//   g_next - g_wm <= T + kReclaimThreshold  (T = thread count; each thread
// has at most one outstanding slot claim), so kLogSlots must exceed that.
static constexpr size_t kLogBits = 17;          // 2^17 ring entries
static constexpr uint64_t kLogSlots = 1ULL << kLogBits;
static constexpr uint64_t kLogMask = kLogSlots - 1;
static constexpr uint32_t kMaxInlineWs = 8;     // inline write entries per log entry
static constexpr uint64_t kReclaimThreshold = 1ULL << 14; // reclaim when window exceeds

// Index table: open addressing, addr -> newest committed writer slot.
// A bucket with addr == 0 is empty; slot is the writer slot (>= 1) or -1
// while an insert is in progress.
static constexpr size_t kIndexBits = 20;
static constexpr uint64_t kIndexSlots = 1ULL << kIndexBits;

// ── Log entry state ────────────────────────────────────────────────
enum LogState : uint32_t {
	LS_FREE = 0,
	LS_PROGRESS = 1,
	LS_COMMITTED = 2,
	LS_ABORTED = 3,
};

struct LogWrite {
	void *addr;
	uint32_t type; // ValueType as uint32 (kept small for packing)
	any_type_t val;
};

struct LogEntry {
	std::atomic<uint32_t> state;  // LogState
	std::atomic<uint32_t> tag;    // owning slot (detects recycling)
	uint32_t ws_count;
	LogWrite ws[kMaxInlineWs];
	LogWrite *overflow;           // heap write-set for ws_count > kMaxInlineWs
};

// ── Index table ────────────────────────────────────────────────────
struct IndexBucket {
	std::atomic<uint64_t> addr;
	std::atomic<int64_t> slot;
};

extern std::atomic<uint64_t> g_next;
extern std::atomic<uint64_t> g_wm;
extern std::atomic<uint32_t> g_commit_lock;
extern LogEntry g_log[kLogSlots];
extern IndexBucket g_index[kIndexSlots];
extern stm::BloomFilter<64> g_dirty;
extern std::atomic<uint64_t> g_tm_abort_count;

// ── Per-transaction state (thread-local) ───────────────────────────
struct ReadLogEntry {
	ValueType type;
	void *addr;
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
	uint64_t slot = 0;
	bool active = false;
	bool aborted = false;
	int abort_count = 0;
	std::vector<ReadLogEntry> read_set;
	std::vector<WriteLogEntry> write_set;

	void reset()
	{
		slot = 0;
		active = false;
		aborted = false;
		abort_count = 0;
		read_set.clear();
		write_set.clear();
	}
};

extern __thread sigjmp_buf *jmpbuf;
extern __thread Transaction *current_tx;

static void setjmp(sigjmp_buf *buf) { jmpbuf = buf; }

inline bool iseq(any_type_t a, any_type_t b) { return a.u8 == b.u8; }

// ── Index helpers ──────────────────────────────────────────────────
inline uint64_t hash_addr(uint64_t a) { return stm::bloom_mix64(a); }

// Newest committed writer slot for addr, or -1.
inline int64_t index_lookup(uint64_t addr)
{
	uint64_t h = hash_addr(addr) & (kIndexSlots - 1);
	for (uint64_t i = 0; i < kIndexSlots; i++) {
		const IndexBucket &b = g_index[(h + i) & (kIndexSlots - 1)];
		uint64_t a = b.addr.load(std::memory_order_acquire);
		if (a == addr) {
			return b.slot.load(std::memory_order_acquire);
		}
		if (a == 0) return -1; // empty bucket: not present
	}
	return -1;
}

// Single-writer (publishers are serialized in slot order): insert or update.
inline void index_insert(uint64_t addr, int64_t slot)
{
	uint64_t h = hash_addr(addr) & (kIndexSlots - 1);
	for (uint64_t i = 0; i < kIndexSlots; i++) {
		IndexBucket &b = g_index[(h + i) & (kIndexSlots - 1)];
		uint64_t a = b.addr.load(std::memory_order_relaxed);
		if (a == addr) {
			b.slot.store(slot, std::memory_order_release);
			return;
		}
		if (a == 0) {
			b.addr.store(addr, std::memory_order_release);
			b.slot.store(slot, std::memory_order_release);
			return;
		}
	}
	fprintf(stderr, "MVLog FATAL: index table full\n");
	std::abort();
}

// ── Initialization ─────────────────────────────────────────────────
inline void init()
{
	g_next.store(1, std::memory_order_release);
	g_wm.store(0, std::memory_order_release);
	for (uint64_t i = 0; i < kLogSlots; i++) {
		g_log[i].state.store(LS_FREE, std::memory_order_relaxed);
		g_log[i].tag.store(0, std::memory_order_relaxed);
		g_log[i].ws_count = 0;
		g_log[i].overflow = nullptr;
	}
	for (uint64_t i = 0; i < kIndexSlots; i++) {
		g_index[i].addr.store(0, std::memory_order_relaxed);
		g_index[i].slot.store(-1, std::memory_order_relaxed);
	}
	g_dirty.clear();
	g_tm_abort_count.store(0, std::memory_order_relaxed);
}

inline void exit() {}

inline void init_thread()
{
	if (!current_tx) {
		current_tx = new Transaction();
	}
	current_tx->reset();
}

inline void exit_thread()
{
	delete current_tx;
	current_tx = nullptr;
}

// ── Generic write-entry scan with type interchange ─────────────────
// Scans `n` write entries (accessor `get(i)` -> tuple<addr, type, val>) for
// one covering `addr` of size `sz`, applying the same interchange rules as
// NOrec-BF's write-set scan (exact-size match, ptr<->u64, wider-to-narrower
// extraction, byte-level merge of UINT8 entries).
template <typename GetT>
inline bool scan_write_entries(
    size_t n, void *addr, ValueType sz, any_type_t &out, GetT &&get)
{
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
	unsigned rs = entrySize(sz);

	for (size_t i = 0; i < n; i++) {
		auto [a, t, v] = get(i);
		if (a != addr) continue;
		unsigned es = entrySize(t);
		if (es == rs && t == sz) {
			out = v;
			return true;
		}
		if (rs == 8 && es == 8 && t != sz) { // ptr <-> u64 interchange
			out = v;
			return true;
		}
		if (es == 8 && rs == 4 && (sz == ValueType::UINT32 || sz == ValueType::FLOAT)) {
			out.u4 = (uint32_t)(v.u8 & 0xFFFFFFFF);
			return true;
		}
		if (es == 8 && rs == 2 && sz == ValueType::UINT16) {
			out.u2 = (uint16_t)(v.u8 & 0xFFFF);
			return true;
		}
		if (es == 8 && rs == 1 && sz == ValueType::UINT8) {
			out.u1 = (uint8_t)(v.u8 & 0xFF);
			return true;
		}
		if (es == 4 && rs == 2 && sz == ValueType::UINT16) {
			out.u2 = (uint16_t)(v.u4 & 0xFFFF);
			return true;
		}
		if (es == 4 && rs == 1 && sz == ValueType::UINT8) {
			out.u1 = (uint8_t)(v.u4 & 0xFF);
			return true;
		}
		if (es == 2 && rs == 1 && sz == ValueType::UINT8) {
			out.u1 = (uint8_t)(v.u2 & 0xFF);
			return true;
		}
	}

	// Byte-merge for wide reads over UINT8 entries.
	if (sz == ValueType::UINT64 || sz == ValueType::POINTER || sz == ValueType::DOUBLE) {
		uint64_t merged = 0;
		bool all_byte = true;
		for (unsigned b = 0; b < 8; b++) {
			void *byte_addr = (void *)((uintptr_t)addr + b);
			bool found = false;
			for (size_t i = 0; i < n; i++) {
				auto [a, t, v] = get(i);
				if (a == byte_addr && t == ValueType::UINT8) {
					merged |= ((uint64_t)v.u1) << (b * 8);
					found = true;
					break;
				}
			}
			if (!found) {
				all_byte = false;
				break;
			}
		}
		if (all_byte) {
			out.u8 = merged;
			return true;
		}
	}
	return false;
}

// ── Log entry helpers ──────────────────────────────────────────────
inline const LogWrite *log_ws(const LogEntry *e, uint32_t &n)
{
	n = e->ws_count;
	return (n > kMaxInlineWs) ? e->overflow : e->ws;
}

inline bool log_value(const LogEntry *e, void *addr, ValueType sz, any_type_t &out)
{
	uint32_t n;
	const LogWrite *ws = log_ws(e, n);
	return scan_write_entries(n, addr, sz, out, [&](size_t i) {
		return std::tuple<void *, ValueType, any_type_t>{
		    ws[i].addr, (ValueType)ws[i].type, ws[i].val};
	});
}

// ── Resolve: newest committed value for addr (read via index + log) ──
// Retries while the entry is recycled under us; terminates because recycling
// implies the slot was folded (g_wm advanced past it) and the `t < w` branch
// then resolves via g_mem.
inline any_type_t resolve(void *addr, ValueType sz)
{
	for (;;) {
		int64_t t = index_lookup(reinterpret_cast<uint64_t>(addr));
		uint64_t w = g_wm.load(std::memory_order_acquire);
		if (t < 0 || (uint64_t)t < w) {
			return read_value_from_addr(addr, sz);
		}
		LogEntry *e = &g_log[t & kLogMask];
		if (e->tag.load(std::memory_order_acquire) != (uint32_t)t) {
			continue; // recycled — re-resolve
		}
		any_type_t out;
		if (!log_value(e, addr, sz, out)) {
			continue; // index changed under us — re-resolve
		}
		if (e->tag.load(std::memory_order_acquire) != (uint32_t)t) {
			continue; // recycled mid-read — re-resolve
		}
		return out;
	}
}

// ── Transaction begin ──────────────────────────────────────────────
// If a previous transaction of this thread is still in flight (aborted via
// longjmp and not yet re-claimed), resolve its slot as ABORTED first.
inline void begin()
{
	auto *tx = current_tx;
	TM_ASSERT(tx, "tx not defined");

	if (tx->active) {
		// Previous attempt aborted (longjmp) without resolving its slot.
		g_log[tx->slot & kLogMask].state.store(LS_ABORTED, std::memory_order_release);
	}

	uint64_t slot = g_next.fetch_add(1, std::memory_order_acq_rel);
	LogEntry *e = &g_log[slot & kLogMask];
	e->tag.store((uint32_t)slot, std::memory_order_relaxed);
	e->state.store(LS_PROGRESS, std::memory_order_release);

	tx->slot = slot;
	tx->active = true;
	tx->aborted = false;
	tx->abort_count = 0;
	tx->read_set.clear();
	tx->write_set.clear();

	TM_EVENT(TX_BEGIN, tx->slot, 0);
}

// ── Abort ──────────────────────────────────────────────────────────
// Clears the sets and longjmps to the retry loop.  The slot stays PROGRESS;
// the retry's begin() resolves it as ABORTED (or reuses it via re-claim).
inline void abort_tx()
{
	auto *tx = current_tx;
	TM_ASSERT(tx, "tx not defined");

	TM_EVENT(TX_ABORT, tx->slot, tx->abort_count);
	tx->abort_count++;
	g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);
	tx->read_set.clear();
	tx->write_set.clear();
	siglongjmp(*jmpbuf, 1);
	TM_ASSERT(false, "Did not jump");
}

// ── Commit ─────────────────────────────────────────────────────────
// Phase 1: wait for every slot in the live window [g_wm, slot) to resolve.
// Phase 2: value-validate the read-set against the final index.
// Phase 3: publish the write-set, update the index, reclaim if due.
inline void commit()
{
	auto *tx = current_tx;
	TM_ASSERT(tx, "tx not defined");
	TM_ASSERT(tx->active, "tx not active");

	uint64_t slot = tx->slot;

	// Phase 1 — wait for predecessors in the live window.
	uint64_t w = g_wm.load(std::memory_order_acquire);
	for (uint64_t s = w; s < slot; s++) {
		LogEntry *e = &g_log[s & kLogMask];
		for (;;) {
			uint32_t st = e->state.load(std::memory_order_acquire);
			// Free (never claimed, e.g. slot 0) or resolved = done; only a
			// claimed-and-unresolved predecessor makes us wait.
			if (st != LS_PROGRESS) break;
			uint32_t tag = e->tag.load(std::memory_order_acquire);
			if (tag != (uint32_t)s) break; // recycled → resolved & folded
			stm::tm_cpu_relax();
		}
	}

	// Commit lock: makes validate → publish atomic w.r.t. other commits.
	// (The TLA+ model's commit is a single atomic action; this serializes it.)
	// Acquired AFTER the predecessor wait so we never hold the lock while
	// waiting on a slot whose resolution requires another commit.
	while (g_commit_lock.exchange(1, std::memory_order_acquire)) {
		stm::tm_cpu_relax();
	}

	// Phase 2 — validate the read-set against the (now frozen) commit state.
	for (auto &r : tx->read_set) {
		any_type_t cur = resolve(r.addr, r.type);
		if (!iseq(cur, r.observed_val)) {
			g_commit_lock.store(0, std::memory_order_release);
			abort_tx();
		}
	}

	// Phase 3 — publish.
	LogEntry *e = &g_log[slot & kLogMask];
	e->ws_count = 0;
	if (tx->write_set.empty()) {
		// Read-only: mark resolved, no index/dirty updates.
		e->state.store(LS_COMMITTED, std::memory_order_release);
		g_commit_lock.store(0, std::memory_order_release);
		tx->reset();
		return;
	}
	if (tx->write_set.size() <= kMaxInlineWs) {
		for (size_t i = 0; i < tx->write_set.size(); i++) {
			e->ws[i].addr = tx->write_set[i].addr;
			e->ws[i].type = (uint32_t)tx->write_set[i].type;
			e->ws[i].val = tx->write_set[i].new_val;
		}
		e->ws_count = (uint32_t)tx->write_set.size();
	} else {
		LogWrite *arr = new LogWrite[tx->write_set.size()];
		for (size_t i = 0; i < tx->write_set.size(); i++) {
			arr[i].addr = tx->write_set[i].addr;
			arr[i].type = (uint32_t)tx->write_set[i].type;
			arr[i].val = tx->write_set[i].new_val;
		}
		e->overflow = arr;
		e->ws_count = (uint32_t)tx->write_set.size();
	}
	TM_EVENT2(COMMIT_WRITEBACK, slot, e->ws_count, 0);

	// Make the entry visible, then update the index (readers resolve only
	// committed entries).
	//
	// Write-through to g_mem as well: peek()-style direct memory reads (and the
	// fast path, when the address is not dirty) expect g_mem to hold the newest
	// committed value.  Ordered before the state publish so a successor that
	// observes this entry COMMITTED in its Phase-1 wait also observes the
	// folded-through values (release/acquire via the state store/load).
	for (auto &w : tx->write_set) {
		write_value_to_addr(w.addr, w.new_val, w.type);
	}
	e->state.store(LS_COMMITTED, std::memory_order_release);
	for (auto &w : tx->write_set) {
		index_insert(reinterpret_cast<uint64_t>(w.addr), (int64_t)slot);
	}

	// Reclamation: fold the folded-out prefix into g_mem, then rotate g_dirty.
	if (slot - g_wm.load(std::memory_order_relaxed) > kReclaimThreshold) {
		for (uint64_t s = w; s < slot; s++) {
			LogEntry *le = &g_log[s & kLogMask];
			if (le->tag.load(std::memory_order_acquire) != (uint32_t)s) {
				continue; // already folded by an earlier reclamation
			}
			if (le->state.load(std::memory_order_acquire) == LS_COMMITTED) {
				uint32_t n;
				const LogWrite *lws = log_ws(le, n);
				for (uint32_t i = 0; i < n; i++) {
					write_value_to_addr(lws[i].addr, lws[i].val, (ValueType)lws[i].type);
				}
			}
		}
		// Folded stores become visible to fast-path readers before the filter
		// is cleared (release fence before the relaxed clear).
		std::atomic_thread_fence(std::memory_order_release);
		g_wm.store(slot, std::memory_order_release);
		g_dirty.clear();
	}

	// Mark this commit's writes dirty so later readers resolve via the log.
	for (auto &w : tx->write_set) {
		g_dirty.insert(reinterpret_cast<uint64_t>(w.addr));
	}

	g_commit_lock.store(0, std::memory_order_release);

	tx->reset();
	TM_EVENT2(COMMIT_SUCCESS, tx->slot, slot, 0);
}

// ── Read / write ───────────────────────────────────────────────────
inline any_type_t read_word(Transaction *tx, void *addr, ValueType sz)
{
	MVLOG_ASSERT_VALID_TX(tx, "read_word");

	// Invalid / kernel-space addresses: return zero (plugin may generate
	// null-derived GEPs).
	if (addr == nullptr || (uintptr_t)addr < 0x100000) {
		any_type_t zero = {};
		return zero;
	}
#ifdef LLVM_TM_PLUGIN
	if (!stm::isTMAddress(addr) && !stm::isTMGlobal(addr)) {
		return read_value_from_addr(addr, sz); // non-TM heap/global: direct
	}
#endif

	// Read-own-writes: scan from the most recent entry.
	if (!tx->write_set.empty()) {
		any_type_t v;
		if (scan_write_entries(
		        tx->write_set.size(), addr, sz, v, [&](size_t i) {
			        const WriteLogEntry &we = tx->write_set[i];
			        return std::tuple<void *, ValueType, any_type_t>{we.addr, we.type,
			                                                         we.new_val};
		        })) {
			return v;
		}
	}

	// FAST PATH: clean Bloom miss — g_mem holds the newest committed value.
	if (!g_dirty.contains(reinterpret_cast<uint64_t>(addr))) {
		// Acquire fence: folded values were made visible before g_dirty was
		// cleared (release fence at reclamation).  Without this, a weak-memory
		// reader could see the cleared filter but a stale g_mem; validation
		// still catches it, but the fence avoids the spurious abort.
		std::atomic_thread_fence(std::memory_order_acquire);
		any_type_t value = read_value_from_addr(addr, sz);
		tx->read_set.push_back({sz, addr, value});
		return value;
	}

	// SLOW PATH: snoop the newest committed writer's log entry.
	any_type_t value = resolve(addr, sz);
	tx->read_set.push_back({sz, addr, value});
	return value;
}

inline void write_word(Transaction *tx, void *addr, any_type_t val, ValueType sz)
{
	MVLOG_ASSERT_VALID_TX(tx, "write_word");

	if (addr == nullptr || (uintptr_t)addr < 0x100000 ||
	    ((uintptr_t)addr >> 47) != 0) {
		return; // invalid address — skip
	}
#ifdef LLVM_TM_PLUGIN
	if (!stm::isTMAddress(addr) && !stm::isTMGlobal(addr)) {
		write_value_to_addr(addr, val, sz); // non-TM heap/global: direct
		return;
	}
#endif

	auto typeSize = [](ValueType t) -> unsigned {
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
	unsigned sz_bytes = typeSize(sz);

	// Scan from the end: update the most recent same-size entry, skip a
	// write covered by a wider entry, otherwise append.
	for (auto it = tx->write_set.rbegin(); it != tx->write_set.rend(); ++it) {
		if (it->addr != addr) continue;
		unsigned es = typeSize(it->type);
		if (es == sz_bytes) {
			it->new_val = val;
			return;
		}
		if (es > sz_bytes) {
			return; // wider entry covers this address
		}
	}
	WriteLogEntry w;
	w.new_val = val;
	w.type = sz;
	w.addr = addr;
	tx->write_set.push_back(w);
	TM_EVENT2(WRITE_SET_INSERT, (uintptr_t)addr, (uint32_t)sz, 0);
}

// ── Typed wrappers ─────────────────────────────────────────────────
template <typename T, ValueType SZ>
inline T tm_read(T *addr)
{
	any_type_t r = read_word(current_tx, (void *)addr, SZ);
	return return_any_type<T>(r);
}

template <typename T, ValueType SZ>
inline void tm_write(T *addr, T val)
{
	any_type_t w;
	fill_any_type(w, &val, SZ);
	write_word(current_tx, (void *)addr, w, SZ);
}

inline uint8_t  tm_read_i1(uint8_t *addr)  { return tm_read<uint8_t, ValueType::UINT8>(addr); }
inline uint16_t tm_read_i2(uint16_t *addr) { return tm_read<uint16_t, ValueType::UINT16>(addr); }
inline uint32_t tm_read_i4(uint32_t *addr) { return tm_read<uint32_t, ValueType::UINT32>(addr); }
inline uint64_t tm_read_i8(uint64_t *addr) { return tm_read<uint64_t, ValueType::UINT64>(addr); }
inline float    tm_read_f4(float *addr)    { return tm_read<float, ValueType::FLOAT>(addr); }
inline double   tm_read_f8(double *addr)   { return tm_read<double, ValueType::DOUBLE>(addr); }
inline void    *tm_read_ptr(void **addr)   { return tm_read<void *, ValueType::POINTER>(addr); }

inline void tm_write_i1(uint8_t *addr, uint8_t val)  { tm_write<uint8_t, ValueType::UINT8>(addr, val); }
inline void tm_write_i2(uint16_t *addr, uint16_t val) { tm_write<uint16_t, ValueType::UINT16>(addr, val); }
inline void tm_write_i4(uint32_t *addr, uint32_t val) { tm_write<uint32_t, ValueType::UINT32>(addr, val); }
inline void tm_write_i8(uint64_t *addr, uint64_t val) { tm_write<uint64_t, ValueType::UINT64>(addr, val); }
inline void tm_write_f4(float *addr, float val)       { tm_write<float, ValueType::FLOAT>(addr, val); }
inline void tm_write_f8(double *addr, double val)     { tm_write<double, ValueType::DOUBLE>(addr, val); }
inline void tm_write_ptr(void **addr, void *val)      { tm_write<void *, ValueType::POINTER>(addr, val); }

} // namespace mvlog
