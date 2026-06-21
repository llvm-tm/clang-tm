/**
 * SPHT — Scalable Persistent Hardware Transactions
 *
 * Uses Intel RTM for hardware-accelerated conflict detection, with a
 * per-thread commit log (PCL) and epoch-based group commit to amortize
 * NVM flush overhead across multiple transactions.
 *
 * Key difference from NV-HTM: the redo log is NOT per-transaction.
 * Instead, a single per-thread PCL accumulates writes across many
 * successive HTM transactions.  Every GROUP_COMMIT_INTERVAL transactions,
 * the entire PCL is flushed to NVM as a group.
 *
 * CPU without RTM: runs in pass-through mode (no TM, direct memory
 * access).  Reads and writes bypass all tracking.
 *
 * ══════════════════════════════════════════════════════════════════
 *  This code runs in the RUNTIME, which is compiled SEPARATELY from
 *  user code and is NEVER fed through the TM plugin.  Every function
 *  and data structure here uses the STANDARD C++ allocator.
 * ══════════════════════════════════════════════════════════════════
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <immintrin.h>
#include <new>

#include "tm_rtm.hpp"
#include <cstdio>

namespace spht
{

using stm::any_type_t;
using stm::ValueType;
using stm::fill_any_type;
using stm::return_any_type;
using stm::read_value_from_addr;
using stm::write_value_to_addr;

constexpr const char *VERSION = "1.0.0-spht";

// ── Configuration ──────────────────────────────────────────────────
constexpr size_t PCL_CAPACITY         = 65536;
constexpr size_t GROUP_COMMIT_INTERVAL = 16;
constexpr int    MAX_RETRIES           = 12;

// ── Log entry types ────────────────────────────────────────────────
enum LogEntryType : uint8_t {
	LOG_WRITE  = 0,  // ordinary write (addr, type, new_val)
	LOG_MALLOC = 1,  // tm_malloc inside TX: addr = pointer, new_val = size
	LOG_FREE   = 2,  // tm_free inside TX: addr = pointer being freed
};

// ── Log entry ──────────────────────────────────────────────────────
struct LogEntry {
	LogEntryType op_type;
	uint8_t      _pad[7];
	void        *addr;
	ValueType    type;
	any_type_t   new_val;   // for LOG_MALLOC: size; for LOG_WRITE: written value
};

// ── Per-Thread Commit Log ──────────────────────────────────────────
struct PCL {
	std::vector<LogEntry> entries;
	size_t epoch_start = 0;

	void reset_epoch() { entries.clear(); epoch_start = 0; }

	void append(void *addr, ValueType type, const any_type_t &val,
	            LogEntryType op_type = LOG_WRITE)
	{
		if (entries.size() >= PCL_CAPACITY) return;
		entries.push_back({op_type, {}, addr, type, val});
	}
};

// ── Transaction state ──────────────────────────────────────────────
struct Transaction {
	bool active = false;
	bool read_only = true;
	int  retry_count = 0;
	uint64_t tx_seq = 0;
	PCL *pcl = nullptr;

	// Set to true once RTM is known to always abort — persists across
	// reset() so subsequent transactions skip RTM entirely.
	bool rtm_broken = false;

	void reset()
	{
		active = false;
		read_only = true;
		retry_count = 0;
	}
};

extern __thread Transaction *current_tx;
extern __thread PCL *g_pcl;
extern __thread sigjmp_buf *jmpbuf;

extern std::atomic<uint64_t> g_num_threads;
extern std::atomic<uint64_t> *g_durable_seqs;

// (reserved for cascade-abort flag)

inline void setjmp(sigjmp_buf *buf) { jmpbuf = buf; }

// ── RTM availability (delegates to common probe) ─────────────────
inline bool rtm_available() { return tm_rtm::available(); }

// =========================================================================
// Global lifecycle
// =========================================================================

inline void init()
{
	g_durable_seqs = nullptr;
	g_num_threads.store(0, std::memory_order_relaxed);
}

inline void exit()
{
	free(g_durable_seqs);
	g_durable_seqs = nullptr;
}

inline void init_thread()
{
	if (!current_tx)
		current_tx = new Transaction();
	current_tx->reset();

	if (!g_pcl) {
		g_pcl = new PCL();
		g_pcl->entries.reserve(4096);
	}

	uint64_t tid = g_num_threads.fetch_add(1, std::memory_order_acq_rel);
	if (tid == 0) {
		g_durable_seqs = (std::atomic<uint64_t> *)calloc(64, sizeof(std::atomic<uint64_t>));
	}
	current_tx->pcl = g_pcl;
	current_tx->tx_seq = 0;

	(void)rtm_available(); // probe once
}

inline void exit_thread()
{
	// Flush any remaining PCL entries on exit
	if (current_tx && g_pcl && !g_pcl->entries.empty()) {
#if defined(__x86_64__) || defined(__i386__)
		for (size_t i = 0; i < g_pcl->entries.size(); i++)
			_mm_clflush(&g_pcl->entries[i]);
		_mm_sfence();
#endif
		g_pcl->reset_epoch();
	}
	delete current_tx;
	current_tx = nullptr;
}

// ── Group commit ─────────────────────────────────────────────────────
static void group_commit(Transaction *tx)
{
	PCL *pcl = tx->pcl;
	if (pcl->entries.empty() || tx->read_only)
		return;

#if defined(__x86_64__) || defined(__i386__)
	for (size_t i = pcl->epoch_start; i < pcl->entries.size(); i++)
		_mm_clflush(&pcl->entries[i]);
	_mm_sfence();

	if (g_durable_seqs) {
		uint64_t idx = tx->tx_seq % 64;
		g_durable_seqs[idx].store(tx->tx_seq, std::memory_order_release);
	}
#endif

	for (size_t i = pcl->epoch_start; i < pcl->entries.size(); i++) {
		LogEntry &e = pcl->entries[i];
		switch (e.op_type) {
		case LOG_WRITE:
			write_value_to_addr(e.addr, e.new_val, e.type);
			break;
		case LOG_MALLOC:
		case LOG_FREE:
			// Alloc/free entries are logged for audit / replay.
			// In the current SPHT design, recovery re-allocates the
			// TM region from scratch, so no replay action is needed.
			break;
		}
	}

	pcl->epoch_start = pcl->entries.size();
}

// =========================================================================
// Transaction begin / commit / abort
// =========================================================================

inline bool begin()
{
	auto *tx = current_tx;
	tx->read_only = true;

	if (tx->rtm_broken || !rtm_available()) {
		tx->active = false;
		return false;
	}

	// Single TSX attempt.  If the TSX aborts (even during body(), which
	// causes the CPU to restore to this _xbegin()), we always fall back
	// to SGL.  We do NOT read the abort reason from a local variable
	// because after a TSX abort during body() the stack may have been
	// reused — only CPU registers (which RTM restores) are reliable.
	//
	// The compiler typically keeps the _xbegin() return value in a
	// register (RAX/EAX).  By inlining the check here without an
	// intervening variable, we avoid spilling to the stack.
	if (_xbegin() == _XBEGIN_STARTED) [[likely]] {
		tx->active = true;
		return true;
	}

	// TSX aborted (either from this _xbegin() or from a concurrent
	// abort during body() of the previous TSX attempt).  Fall back
	// to SGL for correctness.
	tx->active = false;
	tx->rtm_broken = true;
	return false;
}

inline void abort_tx()
{
	if (!rtm_available()) {
		if (current_tx) current_tx->active = false;
		siglongjmp(*jmpbuf, 1);
		return;
	}
	_xabort(1);
	if (current_tx) current_tx->active = false;
	siglongjmp(*jmpbuf, 1);
}

inline bool commit()
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return false;

	_xend();
	tx->tx_seq++;

	if (!tx->read_only && (tx->tx_seq % GROUP_COMMIT_INTERVAL) == 0)
		group_commit(tx);

	tx->reset();
	return true;
}

// =========================================================================
// Read / Write operations
// =========================================================================

template <typename T, ValueType SZ>
inline T tm_read(T *addr)
{
	if (!current_tx || !current_tx->active)
		return *addr;

#ifdef LLVM_TM_PLUGIN
	if (!stm::isTMAddress(addr)) {
		return *addr;
	}
#else
	TM_ASSERT(stm::isTMAddress(addr), "Address not in TM address space");
#endif

	return *addr;
}

template <typename T, ValueType SZ>
inline void tm_write(T *addr, T val)
{
	// Null-address guard
	if (!addr || (uintptr_t)addr < 0x100000 || ((uintptr_t)addr >> 47) != 0)
		return;

#ifdef LLVM_TM_PLUGIN
	if (!stm::isTMAddress(addr)) {
		*addr = val;
		return;
	}
#else
	TM_ASSERT(stm::isTMAddress(addr), "Address not in TM address space");
#endif

	if (!current_tx || !current_tx->active) {
		*addr = val;
		return;
	}

	auto *tx = current_tx;
	tx->read_only = false;

	// Update existing PCL entry for this address (current epoch only)
	for (size_t i = tx->pcl->epoch_start; i < tx->pcl->entries.size(); i++) {
		LogEntry &e = tx->pcl->entries[i];
		if (e.addr == (void *)addr) {
			fill_any_type(e.new_val, &val, SZ);
			e.type = SZ;
			*addr = val;
			return;
		}
	}

	// New PCL entry
	any_type_t w;
	fill_any_type(w, &val, SZ);
	tx->pcl->append((void *)addr, SZ, w);
	*addr = val;
}

// =========================================================================
// Typed wrappers (14 functions matching plugin interface)
// =========================================================================

inline uint8_t  tm_read_i1(uint8_t  *addr) { return tm_read<uint8_t,  ValueType::UINT8>(addr);   }
inline uint16_t tm_read_i2(uint16_t *addr) { return tm_read<uint16_t, ValueType::UINT16>(addr);  }
inline uint32_t tm_read_i4(uint32_t *addr) { return tm_read<uint32_t, ValueType::UINT32>(addr);  }
inline uint64_t tm_read_i8(uint64_t *addr) { return tm_read<uint64_t, ValueType::UINT64>(addr);  }
inline float    tm_read_f4(float    *addr) { return tm_read<float,    ValueType::FLOAT>(addr);   }
inline double   tm_read_f8(double   *addr) { return tm_read<double,   ValueType::DOUBLE>(addr);  }
inline void *   tm_read_ptr(void   **addr) { return tm_read<void *,   ValueType::POINTER>(addr); }

inline void tm_write_i1(uint8_t  *addr, uint8_t  val) { tm_write<uint8_t,  ValueType::UINT8>(addr, val);   }
inline void tm_write_i2(uint16_t *addr, uint16_t val) { tm_write<uint16_t, ValueType::UINT16>(addr, val); }
inline void tm_write_i4(uint32_t *addr, uint32_t val) { tm_write<uint32_t, ValueType::UINT32>(addr, val); }
inline void tm_write_i8(uint64_t *addr, uint64_t val) { tm_write<uint64_t, ValueType::UINT64>(addr, val); }
inline void tm_write_f4(float    *addr, float    val) { tm_write<float,    ValueType::FLOAT>(addr, val);   }
inline void tm_write_f8(double   *addr, double   val) { tm_write<double,   ValueType::DOUBLE>(addr, val);  }
inline void tm_write_ptr(void   **addr, void    *val) { tm_write<void *,   ValueType::POINTER>(addr, val); }

} // namespace spht
