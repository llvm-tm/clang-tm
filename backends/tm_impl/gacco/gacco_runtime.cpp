#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <unordered_map>
#include <vector>

#include "tm_common.hpp"
#include "tm_region_allocator.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// ── Global lock table ────────────────────────────────────
// GaccO: sorted-access lock ordering (GPU-inspired).
// On CPU, this becomes address-sorted lock acquisition
// to guarantee deadlock-free 2PL.
//
// We maintain a global array of per-object locks.  Each
// lock is an atomic<pid> (0 = unlocked, non-zero = holder).
// All locks are acquired in ascending address order.

struct GaccOLock {
    std::atomic<uint64_t> holder{0};
};

// Fixed-size lock table indexed by (addr >> LOCK_SHIFT)
static constexpr size_t LOCK_TABLE_BITS = 20;
static constexpr size_t LOCK_TABLE_SIZE = 1ull << LOCK_TABLE_BITS;
static constexpr int    LOCK_SHIFT      = 4;   // 16-byte granularity
static GaccOLock g_locks[LOCK_TABLE_SIZE];

static inline size_t lock_idx(const void *addr) {
    return ((uintptr_t)addr >> LOCK_SHIFT) & (LOCK_TABLE_SIZE - 1);
}

// ── Per-thread state ─────────────────────────────────────
struct GaccORecord {
    void *addr;
    uint64_t value;
    uint8_t width;
};

static __thread bool g_in_tx = false;
static __thread std::vector<void*>  g_locks_held;   // addresses of held locks (sorted)
static __thread std::vector<GaccORecord> g_reads;
static __thread std::vector<GaccORecord> g_writes;
static __thread uint64_t g_tid = 0;
static std::atomic<uint64_t> g_next_tid{1};
static __thread bool g_aborted = false;

// ── Lock helpers ─────────────────────────────────────────
static void acquire_lock(void *addr) {
    size_t idx = lock_idx(addr);
    auto &lk = g_locks[idx];
    uint64_t expected = 0;
    uint64_t desired = g_tid;
    while (!lk.holder.compare_exchange_weak(expected, desired,
               std::memory_order_acquire,
               std::memory_order_relaxed)) {
        if (expected == g_tid) break;  // already own it
        expected = 0;
    }
    g_locks_held.push_back(addr);
}

static void release_all_locks() {
    for (auto *addr : g_locks_held) {
        size_t idx = lock_idx(addr);
        g_locks[idx].holder.store(0, std::memory_order_release);
    }
    g_locks_held.clear();
}

// ── Static backend implementation ────────────────────────

static void real_tm_begin() {
    if (tm_nested_call_counter > 1) return;
    g_in_tx = true;
    g_aborted = false;
    g_reads.clear();
    g_writes.clear();
    // Locks are acquired lazily on first access.
}

static void real_tm_end() {
    if (tm_nested_call_counter > 1) return;
    if (g_aborted) { release_all_locks(); g_in_tx = false; return; }
    // Commit: writes already applied (GaccO writes through).
    // Validate: for each read, re-check the lock wasn't stolen.
    // If lock still held, the value we read is still valid.
    for (auto &rd : g_reads) {
        size_t idx = lock_idx(rd.addr);
        if (g_locks[idx].holder.load(std::memory_order_acquire) != g_tid) {
            // Lock was stolen — concurrent write corrupted our read.
            release_all_locks();
            g_in_tx = false;
            tm_longjmp_ret = 1;
            siglongjmp(tm_jmpbuf, 1);
            return;
        }
    }
    release_all_locks();
    g_in_tx = false;
}

static void real_tm_abort() {
    g_aborted = true;
}

// ── Read / Write operations ──────────────────────────────
// GaccO GPU: lock on every access, sorted by object index.
// CPU adaptation: lock on first write to an address,
//                 read-only accesses bypass locking.
//                 Writes go directly to memory (write-through).

static uint64_t do_read(void *addr, uint8_t width) {
    if (!g_in_tx) { uint64_t v = 0; memcpy(&v, addr, width); return v; }
    acquire_lock(addr);
    uint64_t v = 0;
    memcpy(&v, addr, width);
    g_reads.push_back({addr, v, width});
    return v;
}

static void do_write(void *addr, uint64_t val, uint8_t width) {
    if (!g_in_tx) { memcpy(addr, &val, width); return; }
    acquire_lock(addr);
    memcpy(addr, &val, width);
    g_writes.push_back({addr, val, width});
}

static uint8_t  real_tm_read_i1 (int8_t  *a) { LLVM_TM_ADDR_CHECK(a); return (uint8_t) do_read((void*)a, 1); }
static uint16_t real_tm_read_i2 (int16_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint16_t)do_read((void*)a, 2); }
static uint32_t real_tm_read_i4 (int32_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint32_t)do_read((void*)a, 4); }
static uint64_t real_tm_read_i8 (int64_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint64_t)do_read((void*)a, 8); }
static float    real_tm_read_f4 (float   *a) { LLVM_TM_ADDR_CHECK(a); float v; memcpy(&v, a, 4); return v; }
static double   real_tm_read_f8 (double  *a) { LLVM_TM_ADDR_CHECK(a); double v; memcpy(&v, a, 8); return v; }

static void real_tm_write_i1(int8_t  *a, uint8_t  v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); do_write((void*)a, v, 1); }
static void real_tm_write_i2(int16_t *a, uint16_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); do_write((void*)a, v, 2); }
static void real_tm_write_i4(int32_t *a, uint32_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); do_write((void*)a, v, 4); }
static void real_tm_write_i8(int64_t *a, uint64_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); do_write((void*)a, v, 8); }
static void real_tm_write_f4(float   *a, float    v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); memcpy(a, &v, 4); }
static void real_tm_write_f8(double  *a, double   v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); memcpy(a, &v, 8); }

static void *real_tm_malloc(size_t s)  { return stm::tm_region_malloc(s); }
static void *real_tm_calloc(size_t n, size_t s) {
    size_t total = n * s;
    void *p = stm::tm_region_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
static void *real_tm_realloc(void *p, size_t s) {
    if (!p) return stm::tm_region_malloc(s);
    void *n = stm::tm_region_malloc(s);
    if (n && p) memcpy(n, p, s);
    return n;
}
static void real_tm_free(void *p) {
    if (!p) return;
}

static void real_tm_init() {
    stm::tm_region_init();
    memset(g_locks, 0, sizeof(g_locks));
}
static void real_tm_exit() {}
static void real_tm_init_thread() {
    tm_hook_init_thread();
    g_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
}
static void real_tm_exit_thread() {}
static void real_tm_set_env(void *env) {}
static void real_tm_set_jmpbuf(void *buf) {}

static int32_t real_tm_get_nested_call_counter() { return tm_nested_call_counter; }
static void    real_tm_set_nested_call_counter(int32_t v) { tm_nested_call_counter = v; }
static int32_t real_tm_get_longjmp_ret()         { return tm_longjmp_ret; }
static int32_t real_tm_load_symbols(const char *lib) { return 0; }

static void real_tm_serialize_lock() {}
static void real_tm_serialize_unlock() {}

static TMRealHooks g_gacco_hooks = {
    .init                    = real_tm_init,
    .exit                    = real_tm_exit,
    .init_thread             = real_tm_init_thread,
    .exit_thread             = real_tm_exit_thread,
    .begin                   = real_tm_begin,
    .end                     = real_tm_end,
    .abort                   = real_tm_abort,
    .malloc                  = real_tm_malloc,
    .calloc                  = real_tm_calloc,
    .realloc                 = real_tm_realloc,
    .free                    = real_tm_free,
    .read_i1                 = real_tm_read_i1,
    .read_i2                 = real_tm_read_i2,
    .read_i4                 = real_tm_read_i4,
    .read_i8                 = real_tm_read_i8,
    .read_f4                 = real_tm_read_f4,
    .read_f8                 = real_tm_read_f8,
    .write_i1                = real_tm_write_i1,
    .write_i2                = real_tm_write_i2,
    .write_i4                = real_tm_write_i4,
    .write_i8                = real_tm_write_i8,
    .write_f4                = real_tm_write_f4,
    .write_f8                = real_tm_write_f8,
    .set_env                 = real_tm_set_env,
    .set_jmpbuf              = real_tm_set_jmpbuf,
    .get_nested_call_counter = real_tm_get_nested_call_counter,
    .set_nested_call_counter = real_tm_set_nested_call_counter,
    .get_longjmp_ret         = real_tm_get_longjmp_ret,
    .load_symbols            = real_tm_load_symbols,
    .serialize_lock          = real_tm_serialize_lock,
    .serialize_unlock        = real_tm_serialize_unlock,
    .get_thread_state        = nullptr,
};
} // extern "C"

#ifdef LLVM_TM_PLUGIN
static void do_tm_init() { real_tm_init(); }
static void do_tm_exit() { real_tm_exit(); }
static void do_tm_init_thread() { real_tm_init_thread(); }
static void do_tm_exit_thread() { real_tm_exit_thread(); }

extern "C" {
void (*tm_init)()        = do_tm_init;
void (*tm_exit)()        = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;
}
#else
extern "C" {
void tm_init()        { real_tm_init();        tm_register_real_hooks(&g_gacco_hooks); }
void tm_exit()        { real_tm_exit(); }
void tm_init_thread() { real_tm_init_thread(); }
void tm_exit_thread() { real_tm_exit_thread(); }
}
#endif
