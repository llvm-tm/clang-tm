#include <algorithm>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "tm_common.hpp"
#include "tm_hooks.hpp"
#include "tm_region_allocator.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// ── Global lock table ────────────────────────────────────
// GaccO: GPU-inspired lock-every-access 2PL.  The GPU original
// sorts accesses by object index (warp-wide); a lazy hook-based
// CPU adaptation cannot sort access order after the fact, so
// instead of promising sorted 2PL (the old comment did) we make
// deadlock-freedom a property of *restart*:
//   * locks are acquired in program order,
//   * acquisition spins only up to a bound, and
//   * exceeding the bound (AB-BA cycle) rolls the transaction's
//     buffered undo log back, releases every held lock, and
//     restarts the transaction.
// Write-through writes are compensated via the undo log, so an
// aborted transaction leaves no visible writes (review-05 G-05).

struct GaccOLock {
	std::atomic<uint64_t> holder{0};
};

// Fixed-size lock table indexed by (addr >> LOCK_SHIFT)
static constexpr size_t LOCK_TABLE_BITS = 20;
static constexpr size_t LOCK_TABLE_SIZE = 1ull << LOCK_TABLE_BITS;
static constexpr int LOCK_SHIFT = 4; // 16-byte granularity
static GaccOLock g_locks[LOCK_TABLE_SIZE];

static inline size_t lock_idx(const void *addr)
{
	return ((uintptr_t)addr >> LOCK_SHIFT) & (LOCK_TABLE_SIZE - 1);
}

// ── Per-thread state ─────────────────────────────────────
struct GaccORecord {
	void *addr;
	uint64_t value;
	uint8_t width;
};

struct GaccOUndo {
	void *addr;
	uint64_t old;
	uint8_t width;
};

static thread_local bool g_in_tx = false;
static thread_local std::vector<void *> g_locks_held;
static thread_local std::vector<GaccORecord> g_reads;
static thread_local std::vector<GaccORecord> g_writes;
static thread_local std::vector<GaccOUndo> g_undo; // first-write-old-values
static thread_local uint64_t g_tid = 0;
static std::atomic<uint64_t> g_next_tid{1};
static thread_local bool g_aborted = false;
static thread_local unsigned g_deadlock_retries = 0;
static thread_local unsigned g_backoff_seed = 1;

// Bounded spin: well beyond any real critical section (each transaction
// body is a handful of memory ops), short enough that an AB-BA cycle is
// detected and broken by restart instead of hanging (review-05 G-05).
static constexpr int GACCO_SPIN_BOUND = 4096;

// ── Lock helpers ─────────────────────────────────────────
// Returns false when the spin bound is exhausted (deadlock detected);
// the caller must then roll back and restart the transaction.
static bool acquire_lock(void *addr)
{
	size_t idx = lock_idx(addr);
	auto &lk = g_locks[idx];
	uint64_t desired = g_tid;
	uint64_t expected = 0;
	if (lk.holder.compare_exchange_weak(expected,
	                                    desired,
	                                    std::memory_order_acquire,
	                                    std::memory_order_relaxed)) {
		g_locks_held.push_back(addr);
		return true;
	}
	if (expected == g_tid) // already own it
		return true;
	for (int i = 0; i < GACCO_SPIN_BOUND; i++) {
		expected = 0;
		if (lk.holder.compare_exchange_weak(expected,
		                                    desired,
		                                    std::memory_order_acquire,
		                                    std::memory_order_relaxed)) {
			g_locks_held.push_back(addr);
			return true;
		}
		if (expected == g_tid)
			return true;
	}
	return false; // spin bound exhausted → restart this transaction
}

static void release_all_locks()
{
	for (auto *addr : g_locks_held) {
		size_t idx = lock_idx(addr);
		g_locks[idx].holder.store(0, std::memory_order_release);
	}
	g_locks_held.clear();
}

// Undo all write-through writes in reverse program order.  g_undo holds
// exactly one entry per touched address (the value before this tx wrote
// it), so repeated writes to the same address undo to the original.
static void rollback_writes()
{
	for (auto it = g_undo.rbegin(); it != g_undo.rend(); ++it)
		memcpy(it->addr, &it->old, it->width);
	g_undo.clear();
	g_writes.clear();
}

// Full abort: undo writes, release locks, restart from tm_begin.
static void real_tm_abort()
{
	if (!g_in_tx)
		return;
	rollback_writes();
	release_all_locks();
	g_reads.clear();
	g_in_tx = false;
	g_aborted = false;
	tm_longjmp_ret = 1;
	siglongjmp(tm_jmpbuf, 1);
}

// ── Static backend implementation ────────────────────────

static void real_tm_begin()
{
	if (tm_nested_call_counter > 1)
		return;
	g_in_tx = true;
	g_aborted = false;
	g_reads.clear();
	g_writes.clear();
	g_undo.clear();
	// Locks are acquired lazily on first access.
}

static void real_tm_end()
{
	if (tm_nested_call_counter > 1)
		return;
	if (g_aborted) {
		rollback_writes();
		release_all_locks();
		g_in_tx = false;
		g_aborted = false;
		return;
	}
	// Commit: writes already applied (GaccO writes through).
	// Validate: for each read, re-check the lock wasn't stolen.
	// If lock still held, the value we read is still valid.
	for (auto &rd : g_reads) {
		size_t idx = lock_idx(rd.addr);
		if (g_locks[idx].holder.load(std::memory_order_acquire) != g_tid) {
			// Lock was stolen — concurrent write corrupted our read.
			rollback_writes();
			release_all_locks();
			g_in_tx = false;
			tm_longjmp_ret = 1;
			siglongjmp(tm_jmpbuf, 1);
			return;
		}
	}
	release_all_locks();
	g_deadlock_retries = 0;
	g_in_tx = false;
}

// ── Read / Write operations ──────────────────────────────
// GaccO GPU: lock on every access, sorted by object index.
// CPU adaptation: lock on first write to an address,
//                 read-only accesses bypass locking.
//                 Writes go directly to memory (write-through).

static void bail_deadlock()
{
	rollback_writes();
	release_all_locks();
	g_reads.clear();
	g_in_tx = false;
	g_aborted = false;
	// Break symmetry: without a backoff, two threads that just killed an
	// AB-BA cycle restart together, collide again, and livelock.
	g_deadlock_retries++;
	unsigned jitter = (rand_r(&g_backoff_seed) % 64) + 1;
	std::this_thread::sleep_for(std::chrono::microseconds(
	    jitter * (g_deadlock_retries > 8 ? 8 : g_deadlock_retries)));
	tm_longjmp_ret = 1;
	siglongjmp(tm_jmpbuf, 1);
}

static uint64_t do_read(void *addr, uint8_t width)
{
	if (!g_in_tx) {
		uint64_t v = 0;
		memcpy(&v, addr, width);
		return v;
	}
	if (!acquire_lock(addr))
		bail_deadlock();
	uint64_t v = 0;
	memcpy(&v, addr, width);
	g_reads.push_back({addr, v, width});
	return v;
}

static void do_write(void *addr, uint64_t val, uint8_t width)
{
	if (!g_in_tx) {
		memcpy(addr, &val, width);
		return;
	}
	if (!acquire_lock(addr))
		bail_deadlock();
	bool first = true;
	for (auto &w : g_writes) {
		if (w.addr == addr) {
			first = false;
			break;
		}
	}
	if (first) {
		uint64_t old = 0;
		memcpy(&old, addr, width);
		g_undo.push_back({addr, old, width});
	}
	memcpy(addr, &val, width);
	g_writes.push_back({addr, val, width});
}

static uint8_t real_tm_read_i1(int8_t *a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (uint8_t)do_read((void *)a, 1);
}
static uint16_t real_tm_read_i2(int16_t *a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (uint16_t)do_read((void *)a, 2);
}
static uint32_t real_tm_read_i4(int32_t *a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (uint32_t)do_read((void *)a, 4);
}
static uint64_t real_tm_read_i8(int64_t *a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (uint64_t)do_read((void *)a, 8);
}
static float real_tm_read_f4(float *a)
{
	LLVM_TM_ADDR_CHECK(a);
	uint64_t r = do_read((void *)a, 4);
	float v;
	memcpy(&v, &r, 4);
	return v;
}
static double real_tm_read_f8(double *a)
{
	LLVM_TM_ADDR_CHECK(a);
	uint64_t r = do_read((void *)a, 8);
	double v;
	memcpy(&v, &r, 8);
	return v;
}

static void real_tm_write_i1(int8_t *a, uint8_t v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, v, 1);
}
static void real_tm_write_i2(int16_t *a, uint16_t v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, v, 2);
}
static void real_tm_write_i4(int32_t *a, uint32_t v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, v, 4);
}
static void real_tm_write_i8(int64_t *a, uint64_t v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, v, 8);
}
static void real_tm_write_f4(float *a, float v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	uint64_t r;
	memcpy(&r, &v, 4);
	do_write((void *)a, r, 4);
}
static void real_tm_write_f8(double *a, double v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	uint64_t r;
	memcpy(&r, &v, 8);
	do_write((void *)a, r, 8);
}

static void *real_tm_read_ptr(void **a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (void *)do_read((void *)a, 8);
}
static void real_tm_write_ptr(void **a, void *v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, (uintptr_t)v, 8);
}

static void *real_tm_malloc(size_t s) { return stm::tm_region_malloc(s); }
static void *real_tm_calloc(size_t n, size_t s)
{
	size_t total = n * s;
	void *p = stm::tm_region_malloc(total);
	if (p)
		memset(p, 0, total);
	return p;
}
static void *real_tm_realloc(void *p, size_t s)
{
	if (!p)
		return stm::tm_region_malloc(s);
	void *n = stm::tm_region_malloc(s);
	if (n && p)
		memcpy(n, p, s);
	return n;
}
static void real_tm_free(void *p)
{
	if (!p)
		return;
}

static void real_tm_init()
{
	stm::tm_region_init();
	memset(g_locks, 0, sizeof(g_locks));
}
static void real_tm_exit() {}
static void real_tm_init_thread()
{
	tm_hook_init_thread();
	g_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
	g_backoff_seed = (unsigned)(g_tid * 2654435761u) ^ (unsigned)(uintptr_t)&g_tid;
	if (g_backoff_seed == 0)
		g_backoff_seed = 1;
}
static void real_tm_exit_thread() {}
static void *real_tm_get_thread_state() { return nullptr; }

static TMRealHooks g_gacco_hooks = {
    .begin = real_tm_begin,
    .end = real_tm_end,
    .malloc = real_tm_malloc,
    .calloc = real_tm_calloc,
    .realloc = real_tm_realloc,
    .free = real_tm_free,
    .read_i1 = (uint8_t (*)(uint8_t *))real_tm_read_i1,
    .read_i2 = (uint16_t (*)(uint16_t *))real_tm_read_i2,
    .read_i4 = (uint32_t (*)(uint32_t *))real_tm_read_i4,
    .read_i8 = (uint64_t (*)(uint64_t *))real_tm_read_i8,
    .read_f4 = real_tm_read_f4,
    .read_f8 = real_tm_read_f8,
    .read_ptr = real_tm_read_ptr,
    .write_i1 = (void (*)(uint8_t *, uint8_t))real_tm_write_i1,
    .write_i2 = (void (*)(uint16_t *, uint16_t))real_tm_write_i2,
    .write_i4 = (void (*)(uint32_t *, uint32_t))real_tm_write_i4,
    .write_i8 = (void (*)(uint64_t *, int64_t))real_tm_write_i8,
    .write_f4 = real_tm_write_f4,
    .write_f8 = real_tm_write_f8,
    .write_ptr = real_tm_write_ptr,
    .get_env = nullptr,
    .set_jmpbuf = nullptr,
    .get_thread_state = real_tm_get_thread_state,
};

#ifdef LLVM_TM_PLUGIN
static void do_tm_init() { real_tm_init(); }
static void do_tm_exit() { real_tm_exit(); }
static void do_tm_init_thread() { real_tm_init_thread(); }
static void do_tm_exit_thread() { real_tm_exit_thread(); }

extern "C" {
void (*tm_init)() = do_tm_init;
void (*tm_exit)() = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;
}
#else
extern "C" {
void tm_init()
{
	real_tm_init();
	tm_register_real_hooks(&g_gacco_hooks);
}
void tm_exit() { real_tm_exit(); }
void tm_init_thread() { real_tm_init_thread(); }
void tm_exit_thread() { real_tm_exit_thread(); }
}
#endif
