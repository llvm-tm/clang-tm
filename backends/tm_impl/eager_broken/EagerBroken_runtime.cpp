/**
 * EagerBroken Runtime — INTENTIONALLY NON-OPAQUE TEACHING BACKEND
 * ================================================================
 *
 * ⚠⚠⚠  THIS BACKEND IS DELIBERATELY BROKEN.  ⚠⚠⚠
 *
 * It implements the classic *trivially eager* STM: speculative writes
 * go straight to memory, an undo log restores old values on abort, and
 * commit performs value-based read-set validation (so the committed
 * projection usually looks serializable).  What it does NOT provide is
 * OPACITY: other threads observe — and can act on — partially built
 * transactional state before the transaction commits or aborts, and
 * they observe the undo of an aborted transaction mid-flight.  That is
 * exactly the failure mode Chapter 2 of the companion book demonstrates
 * with `bin/opaquedemo`: a correct privatization consumer segfaults.
 *
 * Use it ONLY for:
 *   - the book's non-opacity worked example (Chapter 2), and
 *   - the docs/proofs/EagerSTM TLA+ model.
 *
 * Never add it to BACKENDS_TESTS / check-fast / check-all sweeps, and
 * never benchmark with it: results are meaningless because conflicting
 * transactions are allowed to read each other's uncommitted state.
 *
 * Semantics summary (vs NOrec, its optimistic sibling):
 *   read   : raw load, recorded in the validation log.
 *   write  : old value pushed to undo log, then raw store — VISIBLE NOW.
 *   commit : value-based read-set validation; on mismatch the undo log
 *            is applied in reverse, speculative allocations are freed,
 *            and siglongjmp(tm_jmpbuf, 1) restarts the caller's loop.
 *   abort  : invisible to the committed projection, but NOT to concurrent
 *            observers mid-flight — opacity violated by construction.
 */

#include "tm_backend_macros.hpp"
#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "tm_alloc_overrides.hpp"
#include "tm_hooks.hpp"
#include "tm_region_allocator.hpp"
#include "tm_thread_state.hpp"

thread_local bool g_in_tx = false;

static std::atomic<bool> initialized{false};

// Plugin required
struct TMThreadState;
extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;
static thread_local TMThreadState g_tm_state{0, 0};

extern const TMRealHooks g_eager_broken_hooks;

// ═══════════════════════════════════════════════════════════════════
//  Eager core: undo log + validation log.  Writes publish immediately.
// ═══════════════════════════════════════════════════════════════════
namespace eagerbroken
{
struct UndoEntry {
	void *addr;
	uint64_t oldval; // zero-extended previous value
	uint8_t width;   // 1, 2, 4 or 8 bytes
};
struct ReadEntry {
	void *addr;
	uint64_t val; // value observed (raw, possibly by another TX's eager write)
	uint8_t width;
};

static thread_local std::vector<UndoEntry> undo_log;
static thread_local std::vector<ReadEntry> read_log;
static std::atomic<uint64_t> g_abort_count{0};

static inline uint64_t raw_load(void *addr, uint8_t width)
{
	uint64_t out = 0;
	std::memcpy(&out, addr, width);
	return out;
}

static inline void record_read(void *addr, uint8_t width)
{
	if (g_in_tx)
		read_log.push_back({addr, raw_load(addr, width), width});
}

template <typename T>
static inline void eager_write(void *addr, T val)
{
	if (g_in_tx) {
		uint64_t old = 0;
		std::memcpy(&old, addr, sizeof(T));
		undo_log.push_back({addr, old, static_cast<uint8_t>(sizeof(T))});
	}
	std::memcpy(addr, &val, sizeof(T)); // published NOW: no sandboxing!
}

static inline void undo_and_reset()
{
	for (auto it = undo_log.rbegin(); it != undo_log.rend(); ++it)
		std::memcpy(it->addr, &it->oldval, it->width);
	undo_log.clear();
	read_log.clear();
}

// Validation + abort path (called from the commit hook).  Returns false
// when the transaction may commit, never returns on abort (longjmps).
static inline bool validate_or_abort()
{
	for (const ReadEntry &r : read_log) {
		if (raw_load(r.addr, r.width) != r.val) {
			g_abort_count.fetch_add(1, std::memory_order_relaxed);
			undo_and_reset();
			g_in_tx = false;
			tm_clear_spec_allocs(); // abort: free speculative allocations
			tm_clear_deferred_frees();
			siglongjmp(tm_jmpbuf, 1);
			__builtin_unreachable();
		}
	}
	undo_log.clear();
	read_log.clear();
	return false;
}
} // namespace eagerbroken

extern "C" {

static void *real_tm_get_thread_state() { return (void *)&g_tm_state; }

uint64_t tm_eager_abort_count()
{
	return eagerbroken::g_abort_count.load(std::memory_order_relaxed);
}

TM_PLUGIN_LIFECYCLE_VARS()
TM_PLUGIN_LIFECYCLE_FN(do_tm_init, void tm_init())
{
	if (!initialized.load(std::memory_order_relaxed)) {
		initialized.store(true, std::memory_order_seq_cst);
	}
	if (stm::tm_region_init() != 0) {
		fprintf(stderr, "FATAL: tm_region_init() failed\n");
		abort();
	}
	tm_register_real_hooks(&g_eager_broken_hooks);
}

TM_PLUGIN_LIFECYCLE_FN(do_tm_init_thread, void tm_init_thread())
{
	tm_hook_init_thread();
}

TM_PLUGIN_LIFECYCLE_FN(do_tm_exit, void tm_exit())
{
	initialized.store(false, std::memory_order_seq_cst);
	stm::tm_region_destroy();
}

TM_PLUGIN_LIFECYCLE_FN(do_tm_exit_thread, void tm_exit_thread())
{
	tm_hook_exit_thread();
}

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }

void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp() { return 0; }

void tm_set_env(sigjmp_buf *env)
{
	if (env) {
		memcpy(&tm_jmpbuf, env, sizeof(tm_jmpbuf));
	}
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {}

// Plugin-wide wide accesses: raw copies (no sandboxing, by design).
void tm_read_i16(void *addr, void *out)
{
	auto *out_words = static_cast<uint64_t *>(out);
	auto *vaddr = static_cast<volatile uint64_t *>(addr);
	out_words[0] = vaddr[0];
	out_words[1] = vaddr[1];
}
void tm_read_i32(void *addr, void *out)
{
	auto *out_words = static_cast<uint64_t *>(out);
	auto *vaddr = static_cast<volatile uint64_t *>(addr);
	for (int i = 0; i < 4; i++)
		out_words[i] = vaddr[i];
}
void tm_read_i64(void *addr, void *out)
{
	auto *out_words = static_cast<uint64_t *>(out);
	auto *vaddr = static_cast<volatile uint64_t *>(addr);
	for (int i = 0; i < 8; i++)
		out_words[i] = vaddr[i];
}

void *tm_read_z(volatile uint8_t *src, uint64_t len)
{
	void *buf = malloc(len);
	memcpy(buf, (const void *)src, len);
	return buf;
}

void tm_write_i16(void *addr, void *val)
{
	auto *val_words = static_cast<const uint64_t *>(val);
	auto *vaddr = static_cast<volatile uint64_t *>(addr);
	for (int i = 0; i < 2; i++)
		vaddr[i] = val_words[i];
}
void tm_write_i32(void *addr, void *val)
{
	auto *val_words = static_cast<const uint64_t *>(val);
	auto *vaddr = static_cast<volatile uint64_t *>(addr);
	for (int i = 0; i < 4; i++)
		vaddr[i] = val_words[i];
}
void tm_write_i64(void *addr, void *val)
{
	auto *val_words = static_cast<const uint64_t *>(val);
	auto *vaddr = static_cast<volatile uint64_t *>(addr);
	for (int i = 0; i < 8; i++)
		vaddr[i] = val_words[i];
}

void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len)
{
	memcpy((void *)dst, (const void *)src, len);
}

void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len)
{
	memset((void *)addr, val, len);
}

void consume_ptr(volatile void *ptr) { (void)ptr; }

} // extern "C"

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════
static void real_tm_begin()
{
	eagerbroken::undo_log.clear();
	eagerbroken::read_log.clear();
	tm_clear_spec_allocs();
	tm_clear_deferred_frees();
	g_in_tx = true;
}

static void real_tm_end()
{
	// Trivially eager commit: validate the read set against raw memory.
	// Never publishes anything (writes were published already), so the
	// commit lock a real OCC design needs is absent too.
	eagerbroken::validate_or_abort();
	g_in_tx = false;
	tm_flush_spec_allocs();
	tm_flush_deferred_frees();
}

static uint8_t real_tm_read_i1(uint8_t *addr)
{
	eagerbroken::record_read(addr, 1);
	return *addr;
}
static uint16_t real_tm_read_i2(uint16_t *addr)
{
	eagerbroken::record_read(addr, 2);
	return *addr;
}
static uint32_t real_tm_read_i4(uint32_t *addr)
{
	eagerbroken::record_read(addr, 4);
	return *addr;
}
static uint64_t real_tm_read_i8(uint64_t *addr)
{
	eagerbroken::record_read(addr, 8);
	return *addr;
}
static float real_tm_read_f4(float *addr)
{
	eagerbroken::record_read(addr, 4);
	return *addr;
}
static double real_tm_read_f8(double *addr)
{
	eagerbroken::record_read(addr, 8);
	return *addr;
}
static void *real_tm_read_ptr(void **addr)
{
	eagerbroken::record_read(addr, 8);
	return *addr;
}

static void real_tm_write_i1(uint8_t *addr, uint8_t val)
{
	eagerbroken::eager_write(addr, val);
}
static void real_tm_write_i2(uint16_t *addr, uint16_t val)
{
	eagerbroken::eager_write(addr, val);
}
static void real_tm_write_i4(uint32_t *addr, uint32_t val)
{
	eagerbroken::eager_write(addr, val);
}
static void real_tm_write_i8(uint64_t *addr, int64_t val)
{
	eagerbroken::eager_write(addr, val);
}
static void real_tm_write_f4(float *addr, float val)
{
	eagerbroken::eager_write(addr, val);
}
static void real_tm_write_f8(double *addr, double val)
{
	eagerbroken::eager_write(addr, val);
}
static void real_tm_write_ptr(void **addr, void *val)
{
	eagerbroken::eager_write(addr, val);
}

static void *real_tm_malloc(size_t size)
{
	void *p = stm::tm_region_malloc(size);
	if (p) {
		std::memset(p, 0, size);
		tm_track_spec_alloc(p);
	}
	return p;
}
static void *real_tm_calloc(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
	void *p = stm::tm_region_malloc(total);
	if (p) {
		std::memset(p, 0, total);
		tm_track_spec_alloc(p);
	}
	return p;
}
static void *real_tm_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return real_tm_malloc(size);
	void *p = stm::tm_region_malloc(size);
	if (p) {
		std::memcpy(p, ptr, size);
		stm::tm_region_free(ptr);
		tm_track_spec_alloc(p);
	}
	return p;
}
static void real_tm_free(void *ptr)
{
	if (!ptr)
		return;
	tm_untrack_spec_alloc(ptr);
	if (g_in_tx)
		tm_free_append_deferred(ptr);
	else
		stm::tm_region_free(ptr);
}

const TMRealHooks g_eager_broken_hooks = {
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
    .get_thread_state = real_tm_get_thread_state,
};
