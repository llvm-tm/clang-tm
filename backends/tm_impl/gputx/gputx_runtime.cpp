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
#include "tm_platform.hpp"
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

// Committed-write counters, one per 8-byte word, bumped under the commit lock
// after the values are in memory.  Readers pair them with their load (see
// load_stamped) and validate them under the commit lock, so validation does not
// have to trust the value alone: a commit that changed a word and changed it back
// is invisible to a value comparison, which was §35's residual hole.
//
// Word (not lock-slot) granularity is deliberate.  Locks alias at 16 bytes, so
// slot-sized versions would make every reader of the other counters in a slot
// abort whenever one of them commits; at 8 bytes that spurious conflict surface
// halves.  And it makes coverage exact: a naturally aligned 1/2/4/8-byte access
// always lies within one word, and for an unaligned one that straddles a word
// boundary, load_stamped and the validation loop below look at BOTH endpoint words,
// so a write to either is seen.
static std::atomic<uint64_t> g_versions[LOCK_TABLE_SIZE];

static inline size_t word_idx(const void *addr)
{
	return ((uintptr_t)addr >> 3) & (LOCK_TABLE_SIZE - 1);
}

// Width-exact atomic access (backends/tm_impl/common/tm_word_access.hpp).  A
// committing thread stores transactional words in place while a lock-free reader
// may be loading the very same word, and a plain load racing a plain store is
// undefined behaviour regardless of the protocol being sound — that is what
// ThreadSanitizer reported here before these helpers were used.  On x86-64 the
// generated instruction is the same.
static uint64_t load_atomic(const void *addr, uint8_t width)
{
	switch (width) {
	case 1:
		return tm_word::load_relaxed((const uint8_t *)addr);
	case 2:
		return tm_word::load_relaxed((const uint16_t *)addr);
	case 4:
		return tm_word::load_relaxed((const uint32_t *)addr);
	case 8:
		return tm_word::load_relaxed((const uint64_t *)addr);
	default: {
		// Only 1/2/4/8 reach a TM access hook; keep anything else working the
		// slow way rather than pretending to be atomic about it.
		uint64_t v = 0;
		memcpy(&v, addr, width);
		return v;
	}
	}
}

static void store_atomic(void *addr, uint64_t val, uint8_t width)
{
	switch (width) {
	case 1:
		tm_word::store_release((uint8_t *)addr, (uint8_t)val);
		return;
	case 2:
		tm_word::store_release((uint16_t *)addr, (uint16_t)val);
		return;
	case 4:
		tm_word::store_release((uint32_t *)addr, (uint32_t)val);
		return;
	case 8:
		tm_word::store_release((uint64_t *)addr, val);
		return;
	default:
		memcpy(addr, &val, width);
		return;
	}
}

// Sentinel for a read whose version sample stayed torn (someone kept publishing
// into one of its words while we were loading).  Real versions never reach it.
static constexpr uint64_t kVersionUnstable = ~0ull;

// Sample (versions, value, versions) the way a seqlock reader does.  The fences
// keep the value load from being hoisted above the first version reads or sunk
// below the second.  Equal versions mean we saw no publish across our load.
//
// Be precise about what that buys: the sample is a *precision* device, not the
// safety device.  A writer bumps with release ordering only after its stores have
// landed, so a reader that never observes the new version gets no synchronises-with
// edge, and on real hardware it is permitted to load the new value while its second
// version read still sees the old one — a sample that looks stable but records a
// version too low.  What makes that harmless is where validation happens: under the
// commit lock, whose acquire/release pairing makes every bump performed under it
// visible to the next holder, and versions only ever increase.  So a too-low sample
// fails the version comparison and aborts; it cannot commit.  The sample only decides
// whether we abort *unnecessarily*.
static bool load_stamped(void *addr,
                         uint8_t width,
                         uint64_t *val,
                         uint64_t *ver_lo,
                         uint64_t *ver_hi)
{
	const size_t lo = word_idx(addr);
	const size_t hi = word_idx(static_cast<const char *>(addr) + (width ? width - 1 : 0));
	uint64_t lo1 = g_versions[lo].load(std::memory_order_relaxed);
	uint64_t hi1 = g_versions[hi].load(std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_acquire);
	uint64_t value = load_atomic(addr, width);
	std::atomic_thread_fence(std::memory_order_acquire);
	uint64_t hi2 = g_versions[hi].load(std::memory_order_relaxed);
	uint64_t lo2 = g_versions[lo].load(std::memory_order_relaxed);
	*val = value;
	*ver_lo = lo1;
	*ver_hi = hi1;
	return lo1 == lo2 && hi1 == hi2;
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
struct GPUTXRead {
	void *addr;
	uint64_t val;
	uint8_t width;
	uint64_t ver_lo; // version of the access's first word (see load_stamped)
	uint64_t ver_hi; // version of its last word; equal to ver_lo when aligned
};
static thread_local std::vector<GPUTXRead>
    g_reads; // (addr, observed value, width, version)
static thread_local std::vector<GPUTXOp> g_writes; // (addr, newest value, width)
struct GPUTXBufEntry {
	uint64_t val;
	uint8_t width;
};
static thread_local std::unordered_map<void *, GPUTXBufEntry> g_write_buffer;
static thread_local unsigned g_backoff_seed = 1;

static std::atomic<uint64_t> g_next_tid{1};
static std::atomic<uint64_t> g_clock{0}; // global timestamp for ranking

// ── Commit lock ──────────────────────────────────────────
// Reads take no locks, so validating them is only meaningful if nothing can
// commit between the validation and the write-back.  A write lock excludes
// other writers of THAT address only: an address in our READ set can be
// committed by someone else right after we validated it, and we would then
// apply a value derived from a read that is no longer in memory (a lost
// update).  Holding one lock across validate+apply makes the state we
// validate the state we apply on, which is what try_lock_write's comment
// already assumed ("the holder will notice our higher rank when they try to
// commit") without any such check existing.
//
// Lock order is write-locks -> commit-lock, and a commit-lock holder takes no
// write locks (it only validates and applies), so the order cannot cycle.
static std::atomic<uint32_t> g_commit_lock{0};

static void commit_lock_acquire()
{
	while (g_commit_lock.exchange(1, std::memory_order_acquire))
		stm::tm_cpu_relax();
}

static void commit_lock_release() { g_commit_lock.store(0, std::memory_order_release); }

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
		// We have higher priority, but the holder is never asked to step
		// aside: a slot is only ever taken when it is free, so the "holder
		// will notice our higher rank when they try to commit" idea this
		// branch used to describe does not exist — and correctness does not
		// depend on it, because real_tm_end() validates and publishes under
		// the commit lock.  Spin a few iterations in case the holder is
		// about to finish, then abort; the retry gets a higher rank (age
		// escalation), so the wait is bounded.
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

// Abort a transaction that is mid-commit.  Both locks possibly held are
// dropped BEFORE unwinding: siglongjmp leaves no chance to clean up after it.
static void abort_in_commit()
{
	commit_lock_release();
	release_locks();
	g_retries++;
	g_in_tx = false;
	tm_longjmp_ret = 1;
	siglongjmp(tm_jmpbuf, 1);
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
	// Freeze the commit state: no other transaction may publish between our
	// validation and our write-back (see the g_commit_lock comment).
	commit_lock_acquire();
	// Validate reads against the frozen commit state.  The versions are the real
	// test: they change exactly when some transaction publishes into a word the
	// read covers, whether or not the value ended up different.  The value
	// comparison is kept as well because it also covers writes that never bump a
	// version (an out-of-transaction store to transactional memory is the
	// application's own data race, but it should not turn into a silent lost
	// update here).
	for (auto &rd : g_reads) {
		const size_t lo = word_idx(rd.addr);
		const size_t hi = word_idx(static_cast<char *>(rd.addr) +
		                           (rd.width ? rd.width - 1 : 0));
		if (rd.ver_lo == kVersionUnstable ||
		    g_versions[lo].load(std::memory_order_relaxed) != rd.ver_lo ||
		    g_versions[hi].load(std::memory_order_relaxed) != rd.ver_hi) {
			abort_in_commit();
			return;
		}
		if (load_atomic(rd.addr, rd.width) != rd.val) {
			abort_in_commit();
			return;
		}
	}
	// We must still hold the write lock of every address we are about to
	// publish.  try_lock_write() only ever takes a free slot and nothing else
	// clears one, so this holds by construction — checking it is what turns
	// that assumption into a guarantee, because publishing over an address
	// someone else holds is precisely how a committed write goes missing.
	for (auto &w : g_writes) {
		if (g_locks[lock_idx(w.addr)].holder.load(std::memory_order_acquire) != g_tid) {
			abort_in_commit();
			return;
		}
	}
	// Apply writes to memory (write-back), then bump the version of every word we
	// published to.  Values first, versions second, and both while the commit lock
	// is held: that is what lets the next holder's validation see our bump, and
	// what keeps a reader that sampled equal versions from ever having loaded a
	// value out of the middle of this loop.  (We validate the value each
	// transaction OBSERVED, never the buffered new value: memory still held the old
	// one until here, so expecting the buffered value would abort every
	// read-modify-write transaction.)
	for (auto &w : g_writes)
		store_atomic(w.addr, w.val, w.width);
	for (auto &w : g_writes) {
		const size_t lo = word_idx(w.addr);
		const size_t hi = word_idx(static_cast<char *>(w.addr) +
		                           (w.width ? w.width - 1 : 0));
		g_versions[lo].fetch_add(1, std::memory_order_release);
		if (hi != lo)
			g_versions[hi].fetch_add(1, std::memory_order_release);
	}
	release_locks();
	commit_lock_release();
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
	if (!g_in_tx)
		return load_atomic(addr, width);
	auto it = g_write_buffer.find(addr);
	if (it != g_write_buffer.end()) {
		return it->second.val; // read-own-write
	}
	// Retry the (versions, value, versions) sample a few times: on a word another
	// thread publishes to constantly, a single attempt would usually come back torn
	// and fail validation.  A sample that is still torn is recorded as unstable and
	// so aborts rather than committing on a version it cannot vouch for.
	uint64_t v = 0, ver_lo = 0, ver_hi = 0;
	bool stable = false;
	for (int attempt = 0; attempt < 4 && !stable; attempt++)
		stable = load_stamped(addr, width, &v, &ver_lo, &ver_hi);
	if (!stable)
		ver_lo = ver_hi = kVersionUnstable;
	g_reads.push_back({addr, v, width, ver_lo, ver_hi});
	return v;
}

static void do_write(void *addr, uint64_t val, uint8_t width)
{
	if (!g_in_tx) {
		store_atomic(addr, val, width);
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

static uint8_t real_tm_read_i1(uint8_t *a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (uint8_t)do_read((void *)a, 1);
}
static uint16_t real_tm_read_i2(uint16_t *a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (uint16_t)do_read((void *)a, 2);
}
static uint32_t real_tm_read_i4(uint32_t *a)
{
	LLVM_TM_ADDR_CHECK(a);
	return (uint32_t)do_read((void *)a, 4);
}
static uint64_t real_tm_read_i8(uint64_t *a)
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

static void real_tm_write_i1(uint8_t *a, uint8_t v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, v, 1);
}
static void real_tm_write_i2(uint16_t *a, uint16_t v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, v, 2);
}
static void real_tm_write_i4(uint32_t *a, uint32_t v)
{
	LLVM_TM_ADDR_CHECK_WRITE(a, v);
	do_write((void *)a, v, 4);
}
static void real_tm_write_i8(uint64_t *a, int64_t v)
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
    .read_i1 = real_tm_read_i1,
    .read_i2 = real_tm_read_i2,
    .read_i4 = real_tm_read_i4,
    .read_i8 = real_tm_read_i8,
    .read_f4 = real_tm_read_f4,
    .read_f8 = real_tm_read_f8,
    .read_ptr = real_tm_read_ptr,
    .write_i1 = real_tm_write_i1,
    .write_i2 = real_tm_write_i2,
    .write_i4 = real_tm_write_i4,
    .write_i8 = real_tm_write_i8,
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
