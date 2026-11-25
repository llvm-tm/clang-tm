#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "csmv_api.h"
#include "tm_alloc_overrides.hpp"
#include "tm_hooks.hpp"
#include "tm_thread_state.hpp"

// ── TLS variables required by tm_alloc_overrides.hpp ──────────────
extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}
thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;
thread_local FreeNode *g_retired_frees = nullptr;
std::mutex g_retired_global_mutex;
std::unordered_set<void *> g_retired_global_set;

// ── Shared state ─────────────────────────────────────────────────
// CSMV uses a hash table of object entries, each with a version list
// (linked list of VersionNode, newest-first).  Reads traverse the
// list to find the newest version ≤ start_time.  Writes buffer then
// create a new version on commit.

static CSMVObjectEntry *g_csmv_table = nullptr;
static std::atomic<uint64_t> g_csmv_clock{0};
static std::atomic<uint64_t> g_csmv_low_water{0};
static std::atomic<int> g_csmv_active_txns{0};

// ── Server-commit mode (CPU model of the GPU block-0 server) ────
// docs/proofs/GPU_STM_PLAN.md specifies CSMV's protocol as "client-server
// commit: server validates, clients batch writes". On the GPU that server is
// block 0 of the grid; here, when enabled (env CSMV_SERVER_COMMIT=1), a single
// global "server" lock serialises each transaction's entire
// validate -> assign-commit-ts -> install sequence instead of the distributed
// per-entry locks. Clients (the other threads) still do lockless MVCC reads of
// committed versions, exactly as GPU client warps do. This lets the runnable
// T1 differential driver validate the server's serialisation logic on the CPU
// (where it can actually be executed). OFF by default; the default
// distributed per-entry-lock commit path below is unchanged.
static std::mutex g_csmv_server_lock;
static bool g_csmv_server_mode = false;

// ── Per-thread CSMV transaction state ────────────────────────────

static thread_local CSMVThreadTx g_tx;

// GC: version nodes older than low_water_mark can be reclaimed.
// We maintain a per-thread free list of reclaimed nodes.

struct GCNode {
	CSMVVersionNode *node;
	GCNode *next;
};
static thread_local GCNode *g_gc_list = nullptr;

static void gc_reclaim_node(CSMVVersionNode *node)
{
	GCNode *gc = (GCNode *)std::malloc(sizeof(GCNode));
	gc->node = node;
	gc->next = g_gc_list;
	g_gc_list = gc;
}

static void gc_flush()
{
	while (g_gc_list) {
		GCNode *tmp = g_gc_list;
		g_gc_list = g_gc_list->next;
		std::free(tmp->node);
		std::free(tmp);
	}
}

// ── Hash helpers ─────────────────────────────────────────────────

static CSMVObjectEntry *csmv_get_entry(void *addr)
{
	// Hash the FULL address so addresses that differ only in the low bits —
	// e.g. the two int32 halves of one 8-byte word, which `(addr >> 3)` put
	// in the same slot — scatter apart. Linear probing with an owner tag
	// then identifies the entry by address exactly, so every object gets its
	// own version chain. Entries are claimed once and never released (a
	// freed region address reused by the allocator maps to the same entry),
	// so there are no tombstones and a lookup is a clean probe-until-match.
	uint64_t h = (uint64_t)(uintptr_t)addr * 0x9E3779B97F4A7C15ull;
	uint64_t mask = CSMV_TABLE_SIZE - 1;
	uint64_t idx = (h >> (64 - 20)) & mask; // high bits → [0, CSMV_TABLE_SIZE)
	for (uint64_t probe = 0; probe < 1024; probe++) {
		CSMVObjectEntry *e = &g_csmv_table[(idx + probe) & mask];
		void *owner = e->owner.load(std::memory_order_acquire);
		if (owner == addr)
			return e;
		if (owner == nullptr) {
			void *expected = nullptr;
			if (e->owner.compare_exchange_strong(expected,
			                                     addr,
			                                     std::memory_order_acq_rel,
			                                     std::memory_order_acquire))
				return e;
			// CAS failed: `expected` now holds whoever won this slot. If it is
			// OUR OWN address (the common case — another thread first-touching
			// the same hot object claimed this very slot for us), this is our
			// entry; return it. Only if a DIFFERENT address won it do we probe
			// on. Probing on unconditionally here was a bug: the loser would
			// claim a SECOND slot for the same address, splitting its version
			// chain across two entries and silently losing/phantoming updates
			// for whichever thread used the wrong one.
			if (expected == addr)
				return e;
			continue;
		}
		// Occupied by a different address — keep probing.
	}
	fprintf(stderr,
	        "FATAL: csmv_cpu: no slot for address %p within probe limit "
	        "(object table full or over-clustering)\n",
	        addr);
	std::abort();
}

// ── begin / end ──────────────────────────────────────────────────

static void real_tm_begin()
{
	g_tx.start_clock = g_csmv_clock.load(std::memory_order_acquire);
	g_tx.num_reads = 0;
	g_tx.num_writes = 0;
	g_tx.reads.clear();
	g_tx.writes.clear();
	g_tx.reads.reserve(CSMV_MAX_READS);
	g_tx.writes.reserve(CSMV_MAX_WRITES);
	g_csmv_active_txns.fetch_add(1, std::memory_order_relaxed);
	tm_clear_spec_allocs();
	tm_clear_deferred_frees();
}

static void real_tm_end()
{
	uint64_t commit_ts = 0;

	if (g_csmv_server_mode) {
		// Block-0 server model: one global lock serialises the whole
		// validate→commit→install critical section; per-entry locks are
		// unused on this path. Ordering the clock increment and the version
		// installs here is what makes the server the single serialisation
		// point (first-committer-wins on any read-set conflict).
		g_csmv_server_lock.lock();
	} else {
		// ── Lock all write-set entries (in address order) ─────
		// First sort by entry pointer to prevent deadlock
		if (g_tx.num_writes > 1) {
			std::sort(g_tx.writes.begin(),
			          g_tx.writes.end(),
			          [](const CSMVWriteEntry &a, const CSMVWriteEntry &b) {
				          return a.entry < b.entry;
			          });
		}

		// Lock — skip consecutive duplicate entries. Since entries are now
		// keyed one-per-address and write_common dedups by address,
		// duplicates cannot occur, but the guard is kept so lock/unlock stay
		// balanced regardless.
		for (int i = 0; i < g_tx.num_writes; i++) {
			if (i == 0 || g_tx.writes[i].entry != g_tx.writes[i - 1].entry)
				g_tx.writes[i].entry->lock.lock();
		}
	}

	// ── Validate read-set ────────────────────────────────────
	// For each read, check that the head version timestamp hasn't changed
	// (no concurrent writer committed a new version to this object).
	for (int i = 0; i < g_tx.num_reads; i++) {
		CSMVVersionNode *head = g_tx.reads[i].entry->head.load(std::memory_order_acquire);
		uint64_t head_ts = head ? head->timestamp : 0;
		if (head_ts != g_tx.reads[i].observed_ts)
			goto abort_tx;
	}

	// ── Increment global clock ───────────────────────────────
	commit_ts = g_csmv_clock.fetch_add(1, std::memory_order_acq_rel) + 1;

	// ── Create version nodes + write-back to data addresses ──
	for (int i = 0; i < g_tx.num_writes; i++) {
		CSMVObjectEntry *entry = g_tx.writes[i].entry;
		CSMVVersionNode *old_head = entry->head.load(std::memory_order_relaxed);
		void *data_addr = g_tx.writes[i].data_addr;
		uint64_t val = g_tx.writes[i].val;
		uint8_t bytes = g_tx.writes[i].bytes;

		CSMVVersionNode *node = g_tx.writes[i].node;
		if (!node) {
			node = (CSMVVersionNode *)std::malloc(sizeof(CSMVVersionNode));
			g_tx.writes[i].node = node;
		}
		node->timestamp = commit_ts;
		node->value = val;
		node->next = old_head;

		entry->head.store(node, std::memory_order_release);

		// Write-back to actual data address for non-TM readers (peek())
		switch (bytes) {
		case 1:
			__atomic_store_n((uint8_t *)data_addr, (uint8_t)val, __ATOMIC_RELEASE);
			break;
		case 2:
			__atomic_store_n((uint16_t *)data_addr, (uint16_t)val, __ATOMIC_RELEASE);
			break;
		case 4:
			__atomic_store_n((uint32_t *)data_addr, (uint32_t)val, __ATOMIC_RELEASE);
			break;
		case 8:
			__atomic_store_n((uint64_t *)data_addr, val, __ATOMIC_RELEASE);
			break;
		}
	}

	// ── Unlock ───────────────────────────────────────────────
	if (g_csmv_server_mode) {
		g_csmv_server_lock.unlock();
	} else {
		for (int i = g_tx.num_writes - 1; i >= 0; i--) {
			if (i == 0 || g_tx.writes[i].entry != g_tx.writes[i - 1].entry)
				g_tx.writes[i].entry->lock.unlock();
		}
	}

	g_tx.commit_count++;
	g_csmv_active_txns.fetch_sub(1, std::memory_order_relaxed);

	// ── Opportunistic GC every CSMV_GC_INTERVAL commits ──────
	// (skipped in server mode: the sweep uses per-entry locks that the
	//  server commit path does not hold, so it would race the server; the
	//  GPU server kernel has no inline full-table GC either. Driver runs
	//  are short, so leaving version chains un-GCed is fine there.)
	if (!g_csmv_server_mode && g_tx.commit_count % CSMV_GC_INTERVAL == 0) {
		// Update low_water_mark: minimum start_time of all active txns
		// (since we don't have cross-thread tracking here, use a safe
		//  conservative approximation: clock - max_active_window)
		uint64_t now = g_csmv_clock.load(std::memory_order_relaxed);
		uint64_t prev = g_csmv_low_water.load(std::memory_order_relaxed);
		uint64_t new_lwm = now > 1000 ? now - 1000 : 0;
		g_csmv_low_water.store(new_lwm, std::memory_order_relaxed);

		// Sweep version lists: trim nodes older than low_water
		if (new_lwm > prev) {
			for (uint64_t i = 0; i < CSMV_TABLE_SIZE; i++) {
				CSMVObjectEntry *entry = &g_csmv_table[i];
				std::lock_guard<std::mutex> lk(entry->lock);
				CSMVVersionNode *head = entry->head.load(std::memory_order_relaxed);
				if (!head)
					continue;
				// Keep the newest node (always needed) + any with ts >= lwm
				while (head->next && head->next->timestamp < new_lwm) {
					CSMVVersionNode *old = head->next;
					head->next = old->next;
					gc_reclaim_node(old);
				}
			}
			gc_flush();
		}
	}

	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
	return;

abort_tx:
	// Release any held locks (server lock, or the per-entry write locks)
	if (g_csmv_server_mode) {
		g_csmv_server_lock.unlock();
	} else {
		for (int i = g_tx.num_writes - 1; i >= 0; i--) {
			if (i == 0 || g_tx.writes[i].entry != g_tx.writes[i - 1].entry)
				g_tx.writes[i].entry->lock.unlock();
		}
	}
	g_csmv_active_txns.fetch_sub(1, std::memory_order_relaxed);
	siglongjmp(tm_jmpbuf, 1);
}

// ── Read helpers ─────────────────────────────────────────────────
// CSMV read: find the newest version with timestamp ≤ start_clock.
// This NEVER aborts — multi-versioning guarantees a consistent view.

static int find_write(void *data_addr)
{
	for (int i = g_tx.num_writes - 1; i >= 0; i--) {
		if (g_tx.writes[i].data_addr == data_addr)
			return i;
	}
	return -1;
}

static uint64_t read_common(void *data_addr)
{
	// Check write-set first (read-own-writes)
	int wi = find_write(data_addr);
	if (wi >= 0)
		return g_tx.writes[wi].val;

	CSMVObjectEntry *entry = csmv_get_entry(data_addr);

	// Traverse version list to find newest version ≤ start_clock.
	// acquire (not consume): mainstream compilers compile consume as relaxed,
	// which would leave the head->timestamp / ->value / ->next reads below
	// unordered against this load, so a reader could follow a published head
	// to a node whose fields are not yet visible. This walk is lockless (the
	// committer holds the entry lock; readers do not), so it depends on this
	// acquire pairing with the committer's release-store of head.
	CSMVVersionNode *head = entry->head.load(std::memory_order_acquire);
	uint64_t result;
	uint64_t observed;

	if (head == nullptr) {
		// No versions yet — return raw value (initial state before any write)
		// This supports non-TM initialization (e.g., TM<int> x(10))
		result = *(uint64_t *)data_addr;
		observed = 0;
	} else if (head->timestamp <= g_tx.start_clock) {
		// Head is the newest version and within our snapshot → fast path
		result = head->value;
		observed = head->timestamp;
	} else {
		// Head is too new — traverse to find the right version
		CSMVVersionNode *node = head;
		result = 0;
		observed = 0;
		while (node) {
			if (node->timestamp <= g_tx.start_clock) {
				result = node->value;
				observed = node->timestamp;
				break;
			}
			node = node->next;
		}
	}

	// Record read for validation: the OBSERVED NODE's timestamp (shared CSMV
	// convention with the GPU executor).  Commit validation demands the head
	// still equals this ts; recording the head ts instead admits lost updates
	// when the read hit an older-than-head version (see docs/CORRECTNESS_FIXES
	// .md, review-05 G-04).
	g_tx.reads.push_back({entry, observed});
	g_tx.num_reads++;

	return result;
}

// ── Write helper ─────────────────────────────────────────────────

static void write_common(void *data_addr, uint64_t val, uint8_t bytes)
{
	CSMVObjectEntry *entry = csmv_get_entry(data_addr);

	// Check if we already have a write to this address. Entries are now keyed
	// one-per-address (see csmv_get_entry), so matching on data_addr is
	// equivalent to matching on entry; kept explicit for clarity.
	for (int i = 0; i < g_tx.num_writes; i++) {
		if (g_tx.writes[i].entry == entry && g_tx.writes[i].data_addr == data_addr) {
			g_tx.writes[i].val = val;
			g_tx.writes[i].bytes = bytes;
			return;
		}
	}

	// Add new write entry (the set grows — see csmv_api.h on why the old
	// fixed array that dropped overflow entries silently lost updates)
	g_tx.writes.push_back({entry, data_addr, nullptr, val, bytes});
	g_tx.num_writes++;
}

// ── Alloc — use TM region allocator ───────────────────────────────

static void *real_tm_malloc(size_t sz) { return stm::tm_region_malloc(sz); }
static void *real_tm_calloc(size_t n, size_t sz) { return stm::tm_region_calloc(n, sz); }
static void *real_tm_realloc(void *p, size_t sz) { return stm::tm_region_realloc(p, sz); }
static void real_tm_free(void *p) { stm::tm_region_free(p); }

// ── Read stubs ───────────────────────────────────────────────────

static uint8_t real_tm_read_i1(uint8_t *a) { return (uint8_t)read_common(a); }
static uint16_t real_tm_read_i2(uint16_t *a) { return (uint16_t)read_common(a); }
static uint32_t real_tm_read_i4(uint32_t *a) { return (uint32_t)read_common(a); }
static uint64_t real_tm_read_i8(uint64_t *a) { return read_common(a); }
static float real_tm_read_f4(float *a)
{
	uint64_t r = read_common(a);
	float v;
	__builtin_memcpy(&v, &r, 4);
	return v;
}
static double real_tm_read_f8(double *a)
{
	uint64_t r = read_common(a);
	double v;
	__builtin_memcpy(&v, &r, 8);
	return v;
}
static void *real_tm_read_ptr(void **a)
{
	uint64_t r = read_common(a);
	void *v;
	__builtin_memcpy(&v, &r, 8);
	return v;
}

// ── Write stubs ──────────────────────────────────────────────────

static void real_tm_write_i1(uint8_t *a, uint8_t v) { write_common(a, v, 1); }
static void real_tm_write_i2(uint16_t *a, uint16_t v) { write_common(a, v, 2); }
static void real_tm_write_i4(uint32_t *a, uint32_t v) { write_common(a, v, 4); }
static void real_tm_write_i8(uint64_t *a, int64_t v) { write_common(a, (uint64_t)v, 8); }
static void real_tm_write_f4(float *a, float v)
{
	uint64_t r;
	__builtin_memcpy(&r, &v, 4);
	write_common(a, r, 4);
}
static void real_tm_write_f8(double *a, double v)
{
	uint64_t r;
	__builtin_memcpy(&r, &v, 8);
	write_common(a, r, 8);
}
static void real_tm_write_ptr(void **a, void *v)
{
	uint64_t r;
	__builtin_memcpy(&r, &v, 8);
	write_common(a, r, 8);
}

static void *real_tm_get_env() { return nullptr; }
static void real_tm_set_jmpbuf(void *) {}
static void *real_tm_get_thread_state() { return nullptr; }

// ── Hook table ───────────────────────────────────────────────────

static constexpr TMRealHooks g_csmv_hooks = {
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
    .get_env = real_tm_get_env,
    .set_jmpbuf = real_tm_set_jmpbuf,
    .get_thread_state = real_tm_get_thread_state,
};

// ── Lifecycle ────────────────────────────────────────────────────

static void do_tm_init();
static void do_tm_exit();
static void do_tm_init_thread();
static void do_tm_exit_thread();

#ifdef LLVM_TM_PLUGIN
void (*tm_init)() = do_tm_init;
void (*tm_exit)() = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;
static void do_tm_init()
#else
extern "C" void tm_init()
#endif
{
	stm::tm_region_init();

	// CPU model of the GPU block-0 server commit (see g_csmv_server_mode).
	if (const char *e = std::getenv("CSMV_SERVER_COMMIT"))
		g_csmv_server_mode = (e[0] != '\0' && e[0] != '0');

	// Allocate and zero-initialize object table
	g_csmv_table = (CSMVObjectEntry *)std::calloc(CSMV_TABLE_SIZE,
	                                              sizeof(CSMVObjectEntry));
	if (!g_csmv_table) {
		fprintf(stderr, "FATAL: csmv_cpu: calloc object table failed\n");
		std::abort();
	}

	// Initialize each entry's head to nullptr
	for (uint64_t i = 0; i < CSMV_TABLE_SIZE; i++) {
		new (&g_csmv_table[i].owner) std::atomic<void *>{nullptr};
		new (&g_csmv_table[i].lock) std::mutex();
		new (&g_csmv_table[i].head) std::atomic<CSMVVersionNode *>{nullptr};
	}

	g_csmv_clock.store(0, std::memory_order_relaxed);
	g_csmv_low_water.store(0, std::memory_order_relaxed);

	tm_register_real_hooks(&g_csmv_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
extern "C" void tm_exit()
#endif
{
	// Free allocated version nodes
	for (uint64_t i = 0; i < CSMV_TABLE_SIZE; i++) {
		CSMVVersionNode *node = g_csmv_table[i].head.load(std::memory_order_relaxed);
		while (node) {
			CSMVVersionNode *next = node->next;
			std::free(node);
			node = next;
		}
	}
	std::free(g_csmv_table);
	g_csmv_table = nullptr;
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
extern "C" void tm_init_thread()
#endif
{
	tm_hook_init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
extern "C" void tm_exit_thread()
#endif
{
	tm_hook_exit_thread();
}
