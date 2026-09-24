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

// ── GPUTX: GPUTx-style Priority Concurrency Control ──────
//
// Adaptation of the GPUTx rank-based priority scheme to CPU.
// Each transaction has a dynamic priority (rank).  When two
// transactions conflict on an address, the higher-ranked
// transaction continues and the lower-ranked one aborts.
//
// Priority increases with each retry (age-based escalation).
// This prevents starvation and provides forward progress.
//
// Reading does not lock.  Writing acquires an exclusive lock.
// On lock contention, the lower-ranked tx aborts.
//
// The rank ordering replaces the GPU's atomicMax-based
// conflict resolution with a simpler CPU-side abort decision.

// ── Global lock table ────────────────────────────────────
// holder stores the OWNER's tid (never 0 — a tx whose rank would be 0
// must not look unlocked), rank stores the holder's priority for the
// conflict decision.
struct GPUTXLock {
	std::atomic<uint64_t> holder{0}; // 0 = unlocked, non-zero = holder's tid
	std::atomic<uint64_t> rank{0};   // priority of the holder
};

static constexpr size_t LOCK_TABLE_BITS = 20;
static constexpr size_t LOCK_TABLE_SIZE = 1ull << LOCK_TABLE_BITS;
static constexpr int LOCK_SHIFT = 4;
static GPUTXLock g_locks[LOCK_TABLE_SIZE];

static inline size_t lock_idx(const void *addr)
{
	return ((uintptr_t)addr >> LOCK_SHIFT) & (LOCK_TABLE_SIZE - 1);
}

// ── Per-thread state ─────────────────────────────────────
static thread_local bool g_in_tx = false;
static thread_local uint64_t g_rank = 0;    // current priority
static thread_local uint64_t g_retries = 0; // abort count (age)
static thread_local uint64_t g_tid = 0;
struct GPUTXOp {
	void *addr;
	uint64_t val;
	uint8_t width;
};
static thread_local std::vector<GPUTXOp> g_reads;  // (addr, observed value, width)
static thread_local std::vector<GPUTXOp> g_writes; // (addr, newest value, width)
struct GPUTXBufEntry {
	uint64_t val;
	uint8_t width;
};
static thread_local std::unordered_map<void *, GPUTXBufEntry> g_write_buffer;
static thread_local unsigned g_backoff_seed = 1;

static std::atomic<uint64_t> g_next_tid{1};
static std::atomic<uint64_t> g_clock{0}; // global timestamp for ranking

// ── Lock helpers ─────────────────────────────────────────
// GPUTX write-lock with priority-based conflict resolution.
// If someone else holds the lock, compare ranks — lower rank aborts.
static bool try_lock_write(void *addr)
{
	size_t idx = lock_idx(addr);
	auto &lk = g_locks[idx];
	// Reentrant: two of our addresses can map to the same lock slot
	// (LOCK_SHIFT=4), and the holder is our own tid — that is us, not a
	// conflict. Without this the tx aborts against itself and retries
	// forever.
	if (lk.holder.load(std::memory_order_acquire) == g_tid)
		return true;
	uint64_t expected = 0;
	uint64_t desired = g_tid; // tids start at 1; rank 0 txs must not read as free
	if (lk.holder.compare_exchange_weak(expected,
	                                    desired,
	                                    std::memory_order_acquire,
	                                    std::memory_order_relaxed)) {
		lk.rank.store(g_rank, std::memory_order_relaxed);
		return true;
	}
	// Lock held by someone else.
	uint64_t other_rank = lk.rank.load(std::memory_order_relaxed);
	if (g_rank < other_rank) {
		// We have lower priority — abort.
		return false;
	}
	if (g_rank > other_rank) {
		// We have higher priority — spin and retry.
		// (The holder will notice our higher rank when they try to commit
		//  and abort themselves.)
		// For simplicity: spin-wait a few iterations then abort anyway.
		for (int i = 0; i < 100; i++) {
			expected = 0;
			if (lk.holder.compare_exchange_weak(expected,
			                                    desired,
			                                    std::memory_order_acquire,
			                                    std::memory_order_relaxed)) {
				lk.rank.store(g_rank, std::memory_order_relaxed);
				return true;
			}
		}
		return false;
	}
	// Same rank (timestamp collision) — abort, the other wins.
	return false;
}

static void release_locks()
{
	for (auto &w : g_writes) {
		size_t idx = lock_idx(w.addr);
		g_locks[idx].holder.store(0, std::memory_order_release);
	}
}

// ── Static backend implementation ────────────────────────

static void real_tm_begin()
{
	if (tm_nested_call_counter > 1)
		return;
	g_in_tx = true;
	g_reads.clear();
	g_writes.clear();
	g_write_buffer.clear();
	// Rank = (retries << 48) | timestamp
	// Higher retry count = higher priority.
	uint64_t ts = g_clock.fetch_add(1, std::memory_order_relaxed);
	g_rank = (g_retries << 48) | ((ts + 1) & ~(1ull << 63));
}

static void real_tm_end()
{
	if (tm_nested_call_counter > 1)
		return;
	if (!g_in_tx)
		return;
	// Validate reads: for each read, check that no concurrent writer
	// modified the value (compare against write-buffer).
	// Validate each observed read against memory: the value must not
	// have changed since we read it.  (Do NOT validate against the
	// buffered new value for read-then-write addresses — memory still
	// holds the old value until we apply writes below, so expecting the
	// buffered value would abort every read-modify-write transaction.)
	for (auto &rd : g_reads) {
		uint64_t cur = 0;
		memcpy(&cur, rd.addr, rd.width);
		if (cur != rd.val) {
			release_locks();
			g_retries++;
			g_in_tx = false;
			tm_longjmp_ret = 1;
			siglongjmp(tm_jmpbuf, 1);
			return;
		}
	}
	// Apply writes to memory (write-back).
	for (auto &w : g_writes) {
		memcpy(w.addr, &w.val, w.width);
	}
	release_locks();
	g_retries = 0; // reset on success
	g_in_tx = false;
}

static void real_tm_abort()
{
	release_locks();
	g_retries++;
	g_in_tx = false;
}

// ── Read / Write operations ──────────────────────────────
// GPUTX: reads are lock-free (value-based validation at commit).
// Writes acquire a priority-respecting exclusive lock.

static uint64_t do_read(void *addr, uint8_t width)
{
	if (!g_in_tx) {
		uint64_t v = 0;
		memcpy(&v, addr, width);
		return v;
	}
	auto it = g_write_buffer.find(addr);
	if (it != g_write_buffer.end()) {
		return it->second.val; // read-own-write
	}
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
	// For simplicity, always lock on first write to an address.
	auto it = g_write_buffer.find(addr);
	if (it == g_write_buffer.end()) {
		if (!try_lock_write(addr)) {
			// Release locks acquired earlier in THIS transaction before
			// longjmping; leaking them poisons the slot for every other
			// thread (holder stays set, all later writers livelock).
			release_locks();
			g_retries++;
			g_in_tx = false;
			// Backoff breaks restart symmetry; without it an AB-BA cycle
			// livelocks with every participant aborting in lockstep.
			unsigned jitter = (rand_r(&g_backoff_seed) % 64) + 1;
			std::this_thread::sleep_for(std::chrono::microseconds(jitter));
			tm_longjmp_ret = 1;
			siglongjmp(tm_jmpbuf, 1);
			return;
		}
	}
	g_write_buffer[addr] = {val, width};
	// g_writes is the apply list (commit writes these to memory), so a
	// re-write to the same address MUST update the tracked entry — leaving
	// the first value there made commit apply a stale write (G-21: two
	// writes to one address in a TX committed the first, not the last).
	bool already_written = false;
	for (auto &w : g_writes) {
		if (w.addr == addr) {
			w.val = val;
			w.width = width;
			already_written = true;
			break;
		}
	}
	if (!already_written)
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

static TMRealHooks g_gputx_hooks = {
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
	tm_register_real_hooks(&g_gputx_hooks);
}
void tm_exit() { real_tm_exit(); }
void tm_init_thread() { real_tm_init_thread(); }
void tm_exit_thread() { real_tm_exit_thread(); }
}
#endif
