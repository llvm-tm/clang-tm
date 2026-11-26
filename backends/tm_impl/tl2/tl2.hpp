/**
 * TL2 - Full Implementation per Paper Specification
 *
 * Features:
 * - Global version-clock (incremented on each commit)
 * - Versioned write-locks (lock word contains version number)
 * - Version-based validation at commit time
 * - Two-phase locking (commit-time lock acquisition)
 * - Bloom filter for write-set lookup optimization
 * - Multi-type support: 8, 16, 32, 64-bit integers, floats, doubles, pointers
 */

#ifndef TL2_NEW_HPP
#define TL2_NEW_HPP

#include "tm_common.hpp"
#include "tm_event_logger.hpp"
#include "tm_word_access.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <string.h>
#include <type_traits>
#include <vector>

#ifdef TM_ACCEL_GPU
// ACCELTL2: runtime accelerator probe + zero-copy guard table + GPU
// read-set prevalidation (see backends/tm_impl/accel_tl2/).  Never
// defined for the plain TL2 backend — its codegen is unchanged.
#include "accel_probe.hpp"
#endif

namespace tl2
{

constexpr const char *VERSION = "2.0.0-full";

using word_t = uintptr_t;

// Global abort counter — defined in tl2_runtime.cpp
} // namespace tl2

// Shared TLS variables (defined in tm_hooks.cpp, used by all backends)
extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}
extern thread_local bool tm_jmpbuf_initialized;
extern std::atomic<uint64_t> g_tm_abort_count;
namespace tl2
{

using stm::isTMAddress;
// Overload for volatile pointers (TL2 uses volatile throughout)
template <typename T> inline bool isTMAddress(volatile T *addr) noexcept
{
	return stm::isTMAddress(const_cast<const T *>(addr));
}

// Copy jump buffer - needed for external integration with LLVM plugin
inline void copy_jmp_env(sigjmp_buf *dest)
{
	if (dest) {
		memcpy(dest, &tm_jmpbuf, sizeof(sigjmp_buf));
	}
}

// Set jump buffer from external source - allows abort to jump to external setjmp point
inline void set_jmp_env_external(const sigjmp_buf *src)
{
	if (src) {
		memcpy(&tm_jmpbuf, src, sizeof(sigjmp_buf));
		tm_jmpbuf_initialized = true;
	}
}

constexpr unsigned GUARD_TABLE_LOG = 13;
constexpr unsigned GUARD_TABLE_SIZE = 1 << GUARD_TABLE_LOG;
constexpr unsigned BLOOM_FILTER_BITS = 64;

constexpr word_t LOCK_MASK = 1;
constexpr word_t VERSION_MASK = ~LOCK_MASK;

enum class DataType : uint8_t {
	UINT8 = 1,
	UINT16 = 2,
	UINT32 = 4,
	UINT64 = 8,
	PTR = 16,
	FLOAT = 32,
	DOUBLE = 64
};

inline size_t dtype_size(DataType dt)
{
	switch (dt) {
	case DataType::UINT8:
	case DataType::FLOAT:
		return 1;
	case DataType::UINT16:
		return 2;
	case DataType::UINT32:
	case DataType::PTR:
		return 4;
	case DataType::UINT64:
	case DataType::DOUBLE:
		return 8;
	default:
		return 0;
	}
}

struct WriteSetEntry {
	word_t *addr;
	DataType dtype;

	union Values {
		uint8_t u8[2];
		uint16_t u16[2];
		uint32_t u32[2];
		uint64_t u64[2];
		void *ptr[2];
		float f32[2];
		double f64[2];

		Values() { memset(this, 0, sizeof(Values)); }
	} values;

	word_t old_value() const
	{
		switch (dtype) {
		case DataType::UINT8:
			return values.u8[0];
		case DataType::UINT16:
			return values.u16[0];
		case DataType::UINT32:
			return values.u32[0];
		case DataType::FLOAT:
			return values.u32[0];
		case DataType::UINT64:
			return values.u64[0];
		case DataType::DOUBLE:
			return values.u64[0];
		case DataType::PTR:
			return (word_t)values.ptr[0];
		default:
			return 0;
		}
	}

	word_t new_value() const
	{
		switch (dtype) {
		case DataType::UINT8:
			return values.u8[1];
		case DataType::UINT16:
			return values.u16[1];
		case DataType::UINT32:
			return values.u32[1];
		case DataType::FLOAT:
			return values.u32[1];
		case DataType::UINT64:
			return values.u64[1];
		case DataType::DOUBLE:
			return values.u64[1];
		case DataType::PTR:
			return (word_t)values.ptr[1];
		default:
			return 0;
		}
	}

	void set_old(word_t v)
	{
		switch (dtype) {
		case DataType::UINT8:
			values.u8[0] = (uint8_t)v;
			break;
		case DataType::UINT16:
			values.u16[0] = (uint16_t)v;
			break;
		case DataType::UINT32:
			values.u32[0] = (uint32_t)v;
			break;
		case DataType::FLOAT:
			values.u32[0] = (uint32_t)v;
			break;
		case DataType::UINT64:
			values.u64[0] = (uint64_t)v;
			break;
		case DataType::DOUBLE:
			values.u64[0] = (uint64_t)v;
			break;
		case DataType::PTR:
			values.ptr[0] = (void *)v;
			break;
		default:
			break;
		}
	}

	void set_new(word_t v)
	{
		switch (dtype) {
		case DataType::UINT8:
			values.u8[1] = (uint8_t)v;
			break;
		case DataType::UINT16:
			values.u16[1] = (uint16_t)v;
			break;
		case DataType::UINT32:
			values.u32[1] = (uint32_t)v;
			break;
		case DataType::FLOAT:
			values.u32[1] = (uint32_t)v;
			break;
		case DataType::UINT64:
			values.u64[1] = (uint64_t)v;
			break;
		case DataType::DOUBLE:
			values.u64[1] = (uint64_t)v;
			break;
		case DataType::PTR:
			values.ptr[1] = (void *)v;
			break;
		default:
			break;
		}
	}
};

struct ReadSetEntry {
	word_t *guard_addr;
	word_t *data_addr;
	word_t observed_version;
	DataType dtype;
	bool locked_by_me;
};

class Transaction
{
public:
	bool active = false;
	bool aborted = false;
	word_t start_version = 0;
	word_t commit_version = 0;
	std::vector<WriteSetEntry> write_set;
	std::vector<ReadSetEntry> read_set;
	uint64_t bloom_filter = 0;
};

extern thread_local Transaction *current_tx;

class STM
{
private:
	static std::atomic<word_t> g_clock;
#ifdef TM_ACCEL_GPU
	// When an accelerator is available this points into device-shared
	// (fine-grain SVM) memory allocated by accel::shared_guard_table();
	// otherwise at the static backing store (accel_backing()).
	static std::atomic<word_t> *g_guards;

	static std::atomic<word_t> *accel_backing()
	{
		static std::atomic<word_t> backing[GUARD_TABLE_SIZE];
		return backing;
	}
#else
	static std::atomic<word_t> g_guards[GUARD_TABLE_SIZE];
#endif
	static std::atomic<bool> initialized;

	static word_t get_guard_idx(word_t *addr)
	{
		uintptr_t a = (uintptr_t)addr;
		if (sizeof(word_t) == 8) {
			return ((a >> 3) ^ (a >> 48)) & (GUARD_TABLE_SIZE - 1);
		} else {
			return (a >> 2) & (GUARD_TABLE_SIZE - 1);
		}
	}

	static void bloom_set(Transaction *tx, word_t *addr)
	{
		uintptr_t a = (uintptr_t)addr;
		// Set bloom bits for all 8 bytes that the write covers (max width).
		// Without this, sub-offset reads (e.g., UINT32 at addr+4 from a UINT64
		// write at addr+0) miss the bloom filter and skip the write-set scan.
		for (unsigned off = 0; off < 8; off++) {
			uint64_t hash = ((a + off) ^ ((a + off) >> 17)) & 63;
			tx->bloom_filter |= (1ULL << hash);
		}
	}

	static bool bloom_might_contain(Transaction *tx, word_t *addr)
	{
		uintptr_t a = (uintptr_t)addr;
		uint64_t hash = (a ^ (a >> 17)) & 63;
		return (tx->bloom_filter >> hash) & 1ULL;
	}

public:
	static void init()
	{
		if (!initialized.load(std::memory_order_seq_cst)) {
			g_clock.store(1, std::memory_order_relaxed);
#ifdef TM_ACCEL_GPU
			// (Re)acquire the device-shared table whenever we are on the static
			// backing store. accel_teardown() leaves us there precisely because
			// the driver reclaimed the shared block, so a second tm_init() phase
			// has to try again rather than keep the CPU copy forever.
			if (!g_guards || g_guards == accel_backing()) {
				std::atomic<word_t> *shared = nullptr;
				if (accel::probe_available())
					shared = accel::shared_guard_table(GUARD_TABLE_SIZE);
				g_guards = shared ? shared : accel_backing();
			}
#endif
			for (unsigned gi = 0; gi < GUARD_TABLE_SIZE; gi++)
				g_guards[gi].store(0, std::memory_order_relaxed);
			initialized.store(true);
		}
	}

#ifdef TM_ACCEL_GPU
	// Process teardown (tm_exit): move the guard table back onto the static CPU
	// backing store and clear the init latch, so that (a) nothing is still
	// pointing at the fine-grain-SVM block accel::shutdown() hands back to the
	// device driver, and (b) a later tm_init() re-probes and re-binds a shared
	// table. Guard contents are not carried across a teardown: the clock restarts
	// and the TM region is destroyed with the phase, so stale versions and locks
	// would be meaningless (init() zeroes the table again).
	//
	// Only legal at teardown, i.e. when no transaction can be in flight, because
	// the guard-table pointer itself is read lock-free by every guard operation.
	static void accel_teardown()
	{
		g_guards = accel_backing();
		initialized.store(false, std::memory_order_release);
	}
#endif

	static word_t get_clock() { return g_clock.load(std::memory_order_relaxed); }

	static word_t increment_clock()
	{
		return g_clock.fetch_add(1, std::memory_order_release) + 1;
	}

	static word_t get_guard_version(word_t *addr)
	{
		word_t idx = get_guard_idx(addr);
		return (g_guards[idx].load(std::memory_order_acquire) & VERSION_MASK) >> 1;
	}

	static bool is_guard_locked(word_t *addr)
	{
		word_t idx = get_guard_idx(addr);
		return (g_guards[idx].load(std::memory_order_acquire) & LOCK_MASK) != 0;
	}

	static void begin(Transaction *tx)
	{
		tx->start_version = get_clock();
		tx->active = true;
		tx->aborted = false;
		tx->write_set.clear();
		tx->read_set.clear();
		tx->bloom_filter = 0;
		TM_EVENT(TX_BEGIN, (word_t)tx, tx->start_version);
	}

	static bool is_locked_by_me(Transaction *tx, word_t *addr)
	{
		(void)tx;
		word_t idx = get_guard_idx(addr);
		word_t guard = g_guards[idx].load(std::memory_order_acquire);
		return (guard & LOCK_MASK) != 0;
	}

	static bool try_acquire_guard(Transaction *tx, word_t *addr)
	{
		word_t idx = get_guard_idx(addr);
		word_t guard = g_guards[idx].load(std::memory_order_acquire);
		while (!(guard & LOCK_MASK)) {
			word_t desired = guard | LOCK_MASK;
			if (g_guards[idx].compare_exchange_weak(guard,
			                                        desired,
			                                        std::memory_order_acquire,
			                                        std::memory_order_acquire)) {
				return true;
			}
		}
		return false;
	}

	static void release_guard(word_t *addr)
	{
		word_t idx = get_guard_idx(addr);
		g_guards[idx].fetch_and(~LOCK_MASK, std::memory_order_release);
	}

	static void set_guard_version(word_t *addr, word_t version)
	{
		word_t idx = get_guard_idx(addr);
		g_guards[idx].store((version << 1) & VERSION_MASK, std::memory_order_release);
	}

	static word_t read_guard(word_t *guard_addr)
	{
		return reinterpret_cast<std::atomic<word_t> *>(guard_addr)
		    ->load(std::memory_order_acquire);
	}

	// ---- Type conversion helpers ----
	template <typename T> static word_t to_word(T val)
	{
		if constexpr (std::is_integral_v<T>) {
			return static_cast<word_t>(val);
		} else if constexpr (std::is_same_v<T, float>) {
			uint32_t bits;
			memcpy(&bits, &val, sizeof(bits));
			return bits;
		} else if constexpr (std::is_same_v<T, double>) {
			uint64_t bits;
			memcpy(&bits, &val, sizeof(bits));
			return bits;
		} else {
			return reinterpret_cast<word_t>(val);
		}
	}

	template <typename T> static T from_word(word_t val)
	{
		if constexpr (std::is_integral_v<T>) {
			return static_cast<T>(val);
		} else if constexpr (std::is_same_v<T, float>) {
			uint32_t bits = static_cast<uint32_t>(val);
			float result;
			memcpy(&result, &bits, sizeof(result));
			return result;
		} else if constexpr (std::is_same_v<T, double>) {
			double result;
			memcpy(&result, &val, sizeof(result));
			return result;
		} else {
			return reinterpret_cast<T>(val);
		}
	}

	// ---- Generic write implementation ----
	template <typename T, DataType DT, typename AddrT>
	static void write_impl(Transaction *tx, AddrT addr, T val)
	{
		std::atomic_signal_fence(std::memory_order_seq_cst);
		assert(tx && tx->active);

#ifdef LLVM_TM_PLUGIN
		if (!stm::isTMAddress((void *)addr) && !stm::isTMGlobal((const void *)addr)) {
			stm::tm_enforce_tracked((const void *)addr, "write");
			*addr = val;
			return;
		}
#else
		TM_ASSERT(stm::isTMAddress((void *)addr), "Address not in TM address space");
#endif

		// Real byte width of each DataType (NOT dtype_size(), which returns
		// 4 for PTR but PTR write-back writes 8 bytes as word_t).
		auto dataWidth = [](DataType dt) -> unsigned {
			switch (dt) {
			case DataType::UINT8:
				return 1;
			case DataType::UINT16:
				return 2;
			case DataType::UINT32:
				return 4;
			case DataType::FLOAT:
				return 4;
			case DataType::UINT64:
				return 8;
			case DataType::DOUBLE:
				return 8;
			case DataType::PTR:
				return 8;
			default:
				return 0;
			}
		};

		// One entry per address — if any entry already exists at this address,
		// reuse it by updating dtype and new_value.
		for (auto &e : tx->write_set) {
			if (e.addr == (word_t *)addr) {
				e.dtype = DT;
				e.set_new(to_word(val));
				return;
			}
		}

		WriteSetEntry e;
		e.addr = (word_t *)addr;
		e.dtype = DT;
		e.set_new(to_word(val));
		tx->write_set.push_back(e);
		bloom_set(tx, (word_t *)addr);
		TM_EVENT(WRITE_SET_INSERT, (word_t)addr, (word_t)e.dtype);
	}

	// Write functions
	static void write_uint8(Transaction *tx, volatile uint8_t *addr, uint8_t val)
	{
		write_impl<uint8_t, DataType::UINT8>(tx, addr, val);
	}
	static void write_uint16(Transaction *tx, volatile uint16_t *addr, uint16_t val)
	{
		write_impl<uint16_t, DataType::UINT16>(tx, addr, val);
	}
	static void write_uint32(Transaction *tx, volatile uint32_t *addr, uint32_t val)
	{
		write_impl<uint32_t, DataType::UINT32>(tx, addr, val);
	}
	static void write_uint64(Transaction *tx, volatile uint64_t *addr, uint64_t val)
	{
		write_impl<uint64_t, DataType::UINT64>(tx, addr, val);
	}
	static void write_float(Transaction *tx, volatile float *addr, float val)
	{
		write_impl<float, DataType::FLOAT>(tx, addr, val);
	}
	static void write_double(Transaction *tx, volatile double *addr, double val)
	{
		write_impl<double, DataType::DOUBLE>(tx, addr, val);
	}
	static void write_ptr(Transaction *tx, volatile void **addr, void *val)
	{
		write_impl<void *, DataType::PTR>(tx, addr, val);
	}

	// ---- Generic read implementation ----
	template <typename T, DataType DT, typename AddrT>
	static T read_impl(Transaction *tx, AddrT addr)
	{
		assert(tx && tx->active);

#ifdef LLVM_TM_PLUGIN
		if (!stm::isTMAddress((void *)addr) && !stm::isTMGlobal((const void *)addr)) {
			stm::tm_enforce_tracked((const void *)addr, "read");
			if constexpr (std::is_pointer_v<T>) {
				return const_cast<T>(*addr);
			} else {
				return *addr;
			}
		}
#else
		TM_ASSERT(stm::isTMAddress((void *)addr), "Address not in TM address space");
#endif

		if (bloom_might_contain(tx, (word_t *)addr)) {
			// Exact type match (fast path)
			for (auto &e : tx->write_set) {
				if (e.addr == (word_t *)addr && e.dtype == DT) {
					return from_word<T>(e.new_value());
				}
			}
			// Type-interchange fallback: same address, compatible size
			auto swapSize = [](DataType dt) -> unsigned {
				switch (dt) {
				case DataType::UINT8:
					return 1;
				case DataType::UINT16:
					return 2;
				case DataType::UINT32:
					return 4;
				case DataType::FLOAT:
					return 4;
				case DataType::UINT64:
					return 8;
				case DataType::DOUBLE:
					return 8;
				case DataType::PTR:
					return 8;
				default:
					return 0;
				}
			};
			unsigned req_sz = swapSize(DT);
			for (auto &e : tx->write_set) {
				if (e.addr != (word_t *)addr)
					continue;
				unsigned entry_sz = swapSize(e.dtype);
				// Same-size interchange (POINTER ↔ UINT64)
				if (entry_sz == req_sz && entry_sz == 8) {
					return from_word<T>(e.new_value());
				}
				// Wider to narrower extraction
				if (entry_sz == 8 && req_sz == 4) {
					return from_word<T>((uint32_t)(e.new_value() & 0xFFFFFFFF));
				}
				if (entry_sz == 8 && req_sz == 2) {
					return from_word<T>((uint16_t)(e.new_value() & 0xFFFF));
				}
				if (entry_sz == 8 && req_sz == 1) {
					return from_word<T>((uint8_t)(e.new_value() & 0xFF));
				}
				if (entry_sz == 4 && req_sz == 2) {
					return from_word<T>((uint16_t)(e.new_value() & 0xFFFF));
				}
				if (entry_sz == 4 && req_sz == 1) {
					return from_word<T>((uint8_t)(e.new_value() & 0xFF));
				}
				if (entry_sz == 2 && req_sz == 1) {
					return from_word<T>((uint8_t)(e.new_value() & 0xFF));
				}
			}
			// Wider-entry covering: scan for entries that START BEFORE the read
			// address and span past it (e.g., UINT64 at addr+0 covering UINT32 at
			// addr+4).  This happens when the plugin expands memcpy/memset as
			// 8-byte UINT64 writes but individual fields are read at sub-offsets.
			{
				unsigned req_sz2 = swapSize(DT);
				for (auto &e : tx->write_set) {
					if (e.addr == (word_t *)addr)
						continue; // handled above
					unsigned entry_sz = swapSize(e.dtype);
					if (entry_sz == 0 || entry_sz <= req_sz2)
						continue;
					// Check if addr falls within [e.addr, e.addr+entry_sz)
					auto e_start = (uintptr_t)e.addr;
					auto r_addr = (uintptr_t)addr;
					if (r_addr >= e_start && r_addr < e_start + entry_sz) {
						unsigned offset = (unsigned)(r_addr - e_start);
						word_t shifted = e.new_value() >> (offset * 8);
						word_t mask = (req_sz2 == 8) ? ~0ULL
						                             : ((1ULL << (req_sz2 * 8)) - 1);
						return from_word<T>(shifted & mask);
					}
				}
			}
			// General byte-merge: wider read from UINT8 entries at consecutive bytes
			if (swapSize(DT) == 8) {
				uint64_t merged = 0;
				bool all_byte = true;
				for (unsigned i = 0; i < 8; i++) {
					void *byte_addr = (void *)((uintptr_t)addr + i);
					bool found = false;
					for (auto &e : tx->write_set) {
						if (e.addr == (word_t *)byte_addr && e.dtype == DataType::UINT8) {
							merged |= (uint64_t)(e.new_value() & 0xFF) << (i * 8);
							found = true;
							break;
						}
					}
					if (!found) {
						all_byte = false;
						break;
					}
				}
				if (all_byte)
					return from_word<T>(merged);
			}
		}

		word_t idx = get_guard_idx((word_t *)addr);
		word_t guard = g_guards[idx].load(std::memory_order_acquire);
		word_t version = (guard & VERSION_MASK) >> 1;
		bool locked = (guard & LOCK_MASK) != 0;
		TM_EVENT2(READ_LOCK_ACQUIRE, (word_t)addr, (word_t)&g_guards[idx], version);

		ReadSetEntry e;
		e.guard_addr = (word_t *)&g_guards[idx];
		e.data_addr = (word_t *)addr;
		e.observed_version = version;
		e.dtype = DT;
		e.locked_by_me = locked;
		tx->read_set.push_back(e);

		// The data word itself is read WITHOUT holding the guard: a concurrent
		// committer installs committed values into the object in place (the
		// write-back loop in commit()).  That is the intended TL2 protocol — the
		// value is only trusted once commit() re-checks this entry's guard
		// version — but it means the access races a write-back, so it must be
		// expressed as an atomic load: a plain load of a word another thread may
		// be storing is undefined behaviour under the C++ memory model (and
		// ThreadSanitizer reports it as a data race).  Relaxed is sufficient: the
		// ordering that matters is the guard's acquire/release pair in
		// validation, and this value is rejected by version, never by ordering.
		return from_word<T>(to_word(tm_word::load_relaxed(addr)));
	}

	// Read functions
	static uint8_t read_uint8(Transaction *tx, volatile uint8_t *addr)
	{
		return read_impl<uint8_t, DataType::UINT8>(tx, addr);
	}
	static uint16_t read_uint16(Transaction *tx, volatile uint16_t *addr)
	{
		return read_impl<uint16_t, DataType::UINT16>(tx, addr);
	}
	static uint32_t read_uint32(Transaction *tx, volatile uint32_t *addr)
	{
		return read_impl<uint32_t, DataType::UINT32>(tx, addr);
	}
	static uint64_t read_uint64(Transaction *tx, volatile uint64_t *addr)
	{
		return read_impl<uint64_t, DataType::UINT64>(tx, addr);
	}
	static float read_float(Transaction *tx, volatile float *addr)
	{
		return read_impl<float, DataType::FLOAT>(tx, addr);
	}
	static double read_double(Transaction *tx, volatile double *addr)
	{
		return read_impl<double, DataType::DOUBLE>(tx, addr);
	}
	static void *read_ptr(Transaction *tx, volatile void **addr)
	{
		return read_impl<void *, DataType::PTR>(tx, addr);
	}

	static bool commit(Transaction *tx)
	{
		if (!tx || !tx->active)
			return false;

		// Read-only commit optimization (ROCO): skip validation for read-only
		// TXs.  The original TL2 paper specifies that read-only transactions
		// don't need read-set validation because they never write.  Even with
		// an inconsistent snapshot, no data corruption results — the caller
		// simply sees a slightly stale or non-serializable sum.
		// DO NOT validate here — `tm_end()` calls `siglongjmp` on commit
		// failure, which would cause read-only TXs that scan many locations
		// (e.g., `total_transactional()` reading 1024 accounts) to livelock
		// under concurrent writes.
		if (tx->write_set.empty()) {
			tx->active = false;
			return true;
		}

#ifdef TM_ACCEL_GPU
		// ACCELTL2 zero-copy pre-validation: the guard table lives in
		// fine-grain-SVM memory shared with the accelerator, so for large
		// read sets we dispatch a parallel guard-table scan BEFORE sorting
		// and acquiring any locks — a doomed commit aborts without lock
		// churn.  Verdict safety: a guard the device saw locked or with a
		// changed version fails (or is about to fail) Step 5's check in the
		// same state; a transient-lock false positive is possible (writer
		// later aborts), never a missed conflict.  A CLEAN verdict is not
		// trusted: Steps 3-5 below still run in full, so all correctness
		// remains with the CPU code path.
		if (accel::guards_shared() && tx->read_set.size() >= accel::min_entries()) {
			static thread_local std::vector<uint64_t> s_packed;
			s_packed.resize(tx->read_set.size() * 2);
			uint64_t *pk = s_packed.data();
			for (const auto &re : tx->read_set) {
				pk[0] = (uint64_t)(((uintptr_t)re.guard_addr - (uintptr_t)g_guards) /
				                   sizeof(g_guards[0]));
				pk[1] = (uint64_t)re.observed_version;
				pk += 2;
			}
			if (accel::prevalidate(s_packed.data(), tx->read_set.size(), g_guards)) {
				TM_EVENT(TX_ABORT, (word_t)tx, tx->start_version);
				tx->active = false;
				tx->aborted = true;
				return false;
			}
		}
#endif

		// Step 3: Acquire write-set locks, handling guard-table aliasing
		// Sort by address to guarantee deadlock-free lock acquisition
		std::sort(tx->write_set.begin(),
		          tx->write_set.end(),
		          [](const WriteSetEntry &a, const WriteSetEntry &b) {
			          return a.addr < b.addr;
		          });
		bool held_guard[GUARD_TABLE_SIZE] = {false};
		for (auto &e : tx->write_set) {
			word_t idx = get_guard_idx(e.addr);
			if (held_guard[idx])
				continue;
			if (!try_acquire_guard(tx, e.addr)) {
				for (auto &e2 : tx->write_set) {
					word_t idx2 = get_guard_idx(e2.addr);
					if (held_guard[idx2]) {
						TM_EVENT2(LOCK_RELEASE, (word_t)e2.addr, idx2, 0);
						release_guard(e2.addr);
						held_guard[idx2] = false;
					}
				}
				// The reason, before the abort: someone else held this guard.  A
				// TX_ABORT with nothing before it is indistinguishable in the trace
				// from an abort for any other reason, so log the contended
				// acquisition here — addr1 is the data address, addr2 the guard, and
				// data the guard word we found (odd = held by another transaction).
				// Note the convention difference: in TL2 a *successful* commit-path
				// acquisition is logged as COMMIT_LOCK_ACQUIRE below, so this event
				// marks the attempt that failed.  TinySTM logs its successful
				// WRITE_LOCK_ACQUIRE the same way (tinystm_wbctl.hpp:782).
				TM_EVENT2(WRITE_LOCK_ACQUIRE,
				          (word_t)e.addr,
				          (word_t)&g_guards[idx],
				          g_guards[idx].load(std::memory_order_relaxed));
				// Log the abort: TX_BEGIN has no other end on this route.  Without
				// it, tools/stm_bug_tool/invariant_checker.py's lifecycle check sees
				// a TX_BEGIN that never completes and reports a lost transaction
				// (measured: 5 false criticals on a -T 4 TL2 run whose driver oracles
				// were all zero, and 0 TX_ABORT events in 19 210 events of a log
				// where every other aborting backend logs them).
				TM_EVENT(TX_ABORT, (word_t)tx, tx->start_version);
				tx->active = false;
				tx->aborted = true;
				return false;
			}
			held_guard[idx] = true;
			TM_EVENT2(COMMIT_LOCK_ACQUIRE, (word_t)e.addr, idx, 0);
		}

		// Step 4: Increment global version-clock
		tx->commit_version = increment_clock();

		// Step 5: Validate ALL read-set entries — even those in our write-set,
		// because a concurrent writer may have modified them between our read
		// and our commit.
		for (auto &re : tx->read_set) {
			word_t current_guard = read_guard(re.guard_addr);
			word_t current_version = (current_guard & VERSION_MASK) >> 1;

			// Compute guard index from guard_addr pointer to check if WE hold
			// this lock (acquired in step 3).  Our own lock is fine — the
			// version hasn't changed yet (lock was just acquired).  A
			// concurrent writer's lock is a true conflict.
			word_t guard_idx = (word_t)(((uintptr_t)re.guard_addr - (uintptr_t)g_guards) /
			                            sizeof(g_guards[0]));
			bool our_lock = held_guard[guard_idx];

			// Check BOTH lock-state (a concurrent writer holds the guard between
			// lock-acquisition and write-back, but NOT our own lock) AND version
			// (guard was released with a newer version after our read).
			if ((current_guard & LOCK_MASK && !our_lock) ||
			    current_version != re.observed_version) {
				// Which read broke, and what its guard says now.  The reader's own
				// READ_LOCK_ACQUIRE logged the version it observed in `data`, so this
				// event's `data` completes the pair: addr1 is the data address, addr2
				// the guard, and the version found here is the one that invalidated
				// the transaction (or the guard is locked, which this logs as the
				// value seen).  Same payload convention as TinySTM's version checks
				// (tinystm_wbctl.hpp:591).  Without it, 122 of 125 TL2 aborts in one
				// -T 4 run were bare TX_ABORT events, and the trace could not tell a
				// read-set conflict from a guard conflict or from an in-body abort.
				TM_EVENT2(READ_VERSION_CHECK,
				          (word_t)re.data_addr,
				          (word_t)re.guard_addr,
				          current_version);
				for (auto &e : tx->write_set) {
					word_t idx = get_guard_idx(e.addr);
					if (held_guard[idx]) {
						TM_EVENT2(LOCK_RELEASE, (word_t)e.addr, idx, 0);
						release_guard(e.addr);
						held_guard[idx] = false;
					}
				}
				// Same reason as the guard-acquire abort above.
				TM_EVENT(TX_ABORT, (word_t)tx, tx->start_version);
				tx->active = false;
				tx->aborted = true;
				return false;
			}
		}

		// Step 7: Apply writes — write ALL values first without releasing
		// guards, then release each guard ONCE with the commit version.
		// Releasing a guard while other entries share the same guard (aliasing)
		// would allow concurrent readers to see the guard as unlocked with the
		// new version but only a subset of the values written (stale read).
		for (auto &e : tx->write_set) {
			TM_EVENT2(COMMIT_WRITEBACK, (word_t)e.addr, (word_t)e.dtype, 0);
			// Release-ordered atomic stores: lockless readers may be loading this
			// word right now (read_impl uses a relaxed atomic load for exactly
			// that reason), so an in-place plain store would be a data race.
			switch (e.dtype) {
			case DataType::UINT8:
				tm_word::store_release((uint8_t *)e.addr, (uint8_t)e.new_value());
				break;
			case DataType::UINT16:
				tm_word::store_release((uint16_t *)e.addr, (uint16_t)e.new_value());
				break;
			case DataType::UINT32:
				tm_word::store_release((uint32_t *)e.addr, (uint32_t)e.new_value());
				break;
			case DataType::FLOAT: {
				uint32_t bits = (uint32_t)e.new_value();
				float f;
				__builtin_memcpy(&f, &bits, sizeof(f));
				tm_word::store_release((float *)e.addr, f);
			} break;
			case DataType::UINT64:
				tm_word::store_release((uint64_t *)e.addr, e.new_value());
				break;
			case DataType::DOUBLE: {
				uint64_t bits = e.new_value();
				double d;
				__builtin_memcpy(&d, &bits, sizeof(d));
				tm_word::store_release((double *)e.addr, d);
			} break;
			case DataType::PTR:
				tm_word::store_release((word_t *)e.addr, e.new_value());
				break;
			}
		}
		// Release each unique guard exactly once
		{
			bool released_guard[GUARD_TABLE_SIZE] = {false};
			for (auto &e : tx->write_set) {
				word_t idx = get_guard_idx(e.addr);
				if (released_guard[idx])
					continue;
				released_guard[idx] = true;
				g_guards[idx].store((tx->commit_version << 1) & VERSION_MASK,
				                    std::memory_order_release);
				TM_EVENT2(LOCK_RELEASE, (word_t)e.addr, tx->commit_version, 0);
			}
		}

		tx->active = false;
		TM_EVENT(COMMIT_SUCCESS, (word_t)tx, tx->commit_version);
		return true;
	}

	static void abort_tx(Transaction *tx)
	{
		if (!tx)
			return;

		TM_EVENT(TX_ABORT, (word_t)tx, tx->start_version);
		g_tm_abort_count.fetch_add(1, std::memory_order_relaxed);

		// TL2 is write-back — values only reach memory during commit.
		// Guards are only acquired during commit (step 3), and commit handles
		// its own guard release on validation failure. abort_tx() is called
		// either during transaction body (no guards held) or from exit_thread()
		// (no guards held). Do NOT release guards here.

		tx->active = false;
		tx->aborted = true;

		if (tm_jmpbuf_initialized) {
			siglongjmp(tm_jmpbuf, 1);
		}
	}
};

std::atomic<word_t> STM::g_clock{1};
#ifdef TM_ACCEL_GPU
std::atomic<word_t> *STM::g_guards = nullptr; // bound in init(): shared or backing
#else
std::atomic<word_t> STM::g_guards[GUARD_TABLE_SIZE];
#endif
std::atomic<bool> STM::initialized{false};

thread_local Transaction *current_tx = nullptr;

inline void init() { STM::init(); }

#ifdef TM_ACCEL_GPU
inline void accel_teardown() { STM::accel_teardown(); }
#endif

inline void init_thread()
{
	if (!current_tx)
		current_tx = new Transaction();
}

inline void exit_thread()
{
	if (current_tx) {
		if (current_tx->active)
			STM::abort_tx(current_tx);
		delete current_tx;
		current_tx = nullptr;
	}
}

inline bool begin()
{
	init_thread();
	STM::begin(current_tx);
	return true;
}

inline void abort_tx()
{
	if (current_tx)
		STM::abort_tx(current_tx);
}

inline bool commit()
{
	if (!current_tx || !current_tx->active)
		return false;
	return STM::commit(current_tx);
}

inline bool active() { return current_tx && current_tx->active; }
inline bool aborted() { return current_tx && current_tx->aborted; }

inline uint8_t tm_read_i1(volatile uint8_t *addr)
{
	return STM::read_uint8(current_tx, addr);
}
inline uint16_t tm_read_i2(volatile uint16_t *addr)
{
	return STM::read_uint16(current_tx, addr);
}
inline uint32_t tm_read_i4(volatile uint32_t *addr)
{
	return STM::read_uint32(current_tx, addr);
}
inline uint64_t tm_read_i8(volatile uint64_t *addr)
{
	return STM::read_uint64(current_tx, addr);
}
inline float tm_read_f4(volatile float *addr)
{
	return STM::read_float(current_tx, addr);
}
inline double tm_read_f8(volatile double *addr)
{
	return STM::read_double(current_tx, addr);
}
inline void *tm_read_ptr(volatile void **addr) { return STM::read_ptr(current_tx, addr); }

inline void tm_write_i1(volatile uint8_t *addr, uint8_t val)
{
	STM::write_uint8(current_tx, addr, val);
}
inline void tm_write_i2(volatile uint16_t *addr, uint16_t val)
{
	STM::write_uint16(current_tx, addr, val);
}
inline void tm_write_i4(volatile uint32_t *addr, uint32_t val)
{
	STM::write_uint32(current_tx, addr, val);
}
inline void tm_write_i8(volatile uint64_t *addr, uint64_t val)
{
	STM::write_uint64(current_tx, addr, val);
}
inline void tm_write_f4(volatile float *addr, float val)
{
	STM::write_float(current_tx, addr, val);
}
inline void tm_write_f8(volatile double *addr, double val)
{
	STM::write_double(current_tx, addr, val);
}
inline void tm_write_ptr(volatile void **addr, void *val)
{
	STM::write_ptr(current_tx, addr, val);
}

} // namespace tl2

#endif // TL2_NEW_HPP
