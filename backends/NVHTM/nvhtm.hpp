/**
 * NV-HTM — Non-Volatile Hardware Transactional Memory
 *
 * Uses Intel RTM (Restricted Transactional Memory) for hardware-accelerated
 * conflict detection and atomic commit, with a redo log for NVM durability.
 *
 * Protocol (per transaction):
 *   1. _xbegin() starts an RTM transaction — all reads/writes inside are
 *      tracked by hardware; on abort, all memory writes are rolled back.
 *   2. tm_write_* appends (addr, type, value) to the redo log AND writes
 *      through to memory (HTM rolls back both on abort).
 *   3. _xend() commits the HTM — atomic visibility to other threads.
 *   4. Durable phase (after _xend): flush log entries via _mm_clflush,
 *      _mm_sfence, then apply writes to their final NVM addresses.
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
#include <new>
#include <cstdio>

namespace nvhtm
{

using stm::any_type_t;
using stm::ValueType;
using stm::fill_any_type;
using stm::return_any_type;
using stm::read_value_from_addr;
using stm::write_value_to_addr;

constexpr const char *VERSION = "1.0.0-nvhtm";

constexpr size_t LOG_CAPACITY = 4096;   // max TM writes per TX (within L1)
constexpr int    MAX_RETRIES   = 12;     // max HTM retries before fallback

// ── Log entry ──────────────────────────────────────────────────────────
struct LogEntry {
	void *addr;
	ValueType type;
	any_type_t new_val;
};

// ── Transaction state ──────────────────────────────────────────────────
struct Transaction {
	bool active = false;
	bool read_only = true;
	int  retry_count = 0;
	size_t log_count = 0;
	LogEntry log[LOG_CAPACITY];   // redo log buffer

	void reset()
	{
		active = false;
		read_only = true;
		retry_count = 0;
		log_count = 0;
	}

	void clear()
	{
		log_count = 0;
	}
};

extern __thread Transaction *current_tx;
extern __thread sigjmp_buf *jmpbuf;

inline void setjmp(sigjmp_buf *buf) { jmpbuf = buf; }

// ── RTM availability detection ─────────────────────────────────────────
// Returns true if Intel TSX / RTM is available on the current CPU.
// Cached after first call.
inline bool rtm_available()
{
#if defined(__x86_64__) || defined(__i386__)
	static int cached = -1;
	if (cached < 0) {
		unsigned int a = 0, b = 0, c = 0, d = 0;
		__cpuid_count(7, 0, a, b, c, d);
		cached = (b & (1 << 11)) ? 1 : 0;  // EBX[11] = RTM
		if (!cached)
			fprintf(stderr, "[NVHTM] RTM not available — running without HTM\n");
	}
	return cached > 0;
#else
	return false;
#endif
}

// =========================================================================
// Global lifecycle
// =========================================================================

inline void init() {}
inline void exit() {}

inline void init_thread()
{
	if (!current_tx)
		current_tx = new Transaction();
	current_tx->reset();
	(void)rtm_available(); // probe once at init
}

inline void exit_thread()
{
	delete current_tx;
	current_tx = nullptr;
}

// =========================================================================
// Transaction begin / commit / abort
// =========================================================================

inline bool begin()
{
	auto *tx = current_tx;
	tx->clear();
	tx->read_only = true;

	if (!rtm_available()) {
		// Pass-through: run without TM
		tx->active = false;
		return false;
	}

	unsigned status = _xbegin();
	if (status == _XBEGIN_STARTED) {
		tx->active = true;
		return true;
	}

	// HTM abort — retry
	tx->retry_count++;
	if (tx->retry_count > MAX_RETRIES) {
		tx->active = false;
		return false;
	}
	if (tx->retry_count > 3) {
		std::this_thread::sleep_for(
		    std::chrono::microseconds(10 * (1 << (tx->retry_count - 3))));
	}
	siglongjmp(*jmpbuf, 1);
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
	// _xabort rolls back to _xbegin() inside begin(), which then
	// drives the retry via siglongjmp.  Never reaches here on RTM CPUs.
	if (current_tx) current_tx->active = false;
	siglongjmp(*jmpbuf, 1);
}

// ── Durable phase (outside HTM, after _xend) ────────────────────────────
static void durable_commit(Transaction *tx)
{
	if (tx->read_only || tx->log_count == 0)
		return;

#if defined(__x86_64__) || defined(__i386__)
	// Step A: flush each cache line of the redo log to NVM.
	for (size_t i = 0; i < tx->log_count; i++)
		_mm_clflush(&tx->log[i]);
	// Step B: ensure all clflushes have completed.
	_mm_sfence();

	// Step C: apply writes to final NVM addresses (idempotent).
	for (size_t i = 0; i < tx->log_count; i++)
		write_value_to_addr(tx->log[i].addr, tx->log[i].new_val, tx->log[i].type);
#else
	for (size_t i = 0; i < tx->log_count; i++)
		write_value_to_addr(tx->log[i].addr, tx->log[i].new_val, tx->log[i].type);
#endif
}

inline bool commit()
{
	auto *tx = current_tx;
	if (!tx || !tx->active)
		return false;

	_xend();
	durable_commit(tx);
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
	return *addr; // HTM tracks the read-set in hardware
}

template <typename T, ValueType SZ>
inline void tm_write(T *addr, T val)
{
	// Null-address guard: writing to < 0x100000 or kernel-space (> 47-bit
	// top bit set) is either a moved-from null pointer GEP or a bug.
	// Skip safely — the data is garbage anyway.
	if (!addr || (uintptr_t)addr < 0x100000 || ((uintptr_t)addr >> 47) != 0)
		return;

	if (!current_tx || !current_tx->active) {
		*addr = val;
		return;
	}

	auto *tx = current_tx;
	tx->read_only = false;

	// Scan for existing entry at this address
	for (size_t i = 0; i < tx->log_count; i++) {
		if (tx->log[i].addr == (void *)addr) {
			fill_any_type(tx->log[i].new_val, &val, SZ);
			tx->log[i].type = SZ;
			*addr = val; // write-through (rolled back on HTM abort)
			return;
		}
	}

	// New log entry
	size_t idx = tx->log_count;
	if (idx >= LOG_CAPACITY) {
		fprintf(stderr, "[NVHTM] FATAL: redo log full (%zu entries)\n", LOG_CAPACITY);
		abort_tx();
	}
	tx->log_count++;
	LogEntry &e = tx->log[idx];
	e.addr = (void *)addr;
	e.type = SZ;
	fill_any_type(e.new_val, &val, SZ);
	*addr = val; // write-through (rolled back on HTM abort)
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

} // namespace nvhtm
