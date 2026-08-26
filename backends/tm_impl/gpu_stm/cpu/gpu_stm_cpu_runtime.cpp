#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <csetjmp>
#include <atomic>
#include <thread>
#include <vector>

#include "gpu_stm_api.h"
#include "tm_hooks.hpp"
#include "tm_thread_state.hpp"
#include "tm_alloc_overrides.hpp"

// ── TLS variables required by tm_alloc_overrides.hpp ──────────────
thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;
thread_local FreeNode *g_retired_frees = nullptr;
std::mutex g_retired_global_mutex;
std::unordered_set<void *> g_retired_global_set;

extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// ── Shared state (per-backend-process, not per-GPU-warp) ──────────
// This runtime implements PR-STM WITHOUT warp barriers — each thread
// runs independently via TMRealHooks.  The lock table and clock are
// shared across all threads.

static uint32_t *gpu_lock_table = nullptr;
static std::atomic<uint64_t> gpu_global_clock{0};

// ── Per-thread PR-STM state ───────────────────────────────────────

struct [[gnu::aligned(64)]] GpuStmThreadTx {
    uint64_t start_clock;
    int num_reads;
    int num_writes;
    struct { uint32_t lock_idx; uint32_t ver; } reads[PR_STM_MAX_READS];
    struct { uint32_t lock_idx; void *data_addr; uint64_t val; uint8_t bytes; } writes[PR_STM_MAX_WRITES];
};

static thread_local GpuStmThreadTx g_tx;

// ── PR-STM operations ─────────────────────────────────────────────

static void real_tm_begin() {
    g_tx.start_clock = gpu_global_clock.load(std::memory_order_relaxed);
    g_tx.num_reads = 0;
    g_tx.num_writes = 0;
    tm_clear_spec_allocs();
    tm_clear_deferred_frees();
}

static void real_tm_end() {
    uint8_t priority = (uint8_t)((uintptr_t)&g_tx % 255 + 1);
    uint64_t commit_clock = 0;

    // ── VALIDATE (acquire semantics) ──────────────────────────
    for (int i = 0; i < g_tx.num_reads; i++) {
        uint32_t lw = __atomic_load_n(&gpu_lock_table[g_tx.reads[i].lock_idx], __ATOMIC_ACQUIRE);
        if (pr_stm_is_locked(lw) || pr_stm_get_version(lw) != g_tx.reads[i].ver) {
            goto abort_tx;
        }
    }
    // ── ACQUIRE locks (priority-based spin-loop) ──────────────
    // PR-STM: can only acquire if lock is free OR we already hold it (same priority).
    // If another thread holds it (different priority), abort.
    for (int i = 0; i < g_tx.num_writes; i++) {
        uint32_t lock_idx = g_tx.writes[i].lock_idx;
        // Check if we already hold this lock entry (same lock_idx acquired earlier)
        bool already_held = false;
        for (int j = 0; j < i; j++) {
            if (g_tx.writes[j].lock_idx == lock_idx) {
                already_held = true;
                break;
            }
        }
        if (already_held) continue;
        while (true) {
            uint32_t expected = __atomic_load_n(&gpu_lock_table[lock_idx], __ATOMIC_ACQUIRE);
            if (pr_stm_is_locked(expected)) {
                if (pr_stm_get_priority(expected) != priority) {
                    goto abort_tx;
                }
                // Same priority — we already hold it (shouldn't reach here if already_held check works)
                break;
            }
            uint32_t desired = pr_stm_make_entry(priority, pr_stm_get_version(expected), 1);
            if (__atomic_compare_exchange_n(&gpu_lock_table[lock_idx], &expected, desired, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
        }
    }
    // ── RE-VALIDATE after lock acquisition ────────────────────
    // Ensure no concurrent transaction modified our read-set while
    // we were acquiring locks. Check ALL reads (including read-own-write).
    // If another transaction modified an address we read, the version
    // will have changed even if we later wrote to it.
    for (int i = 0; i < g_tx.num_reads; i++) {
        uint32_t read_lock_idx = g_tx.reads[i].lock_idx;
        
        uint32_t lw = __atomic_load_n(&gpu_lock_table[read_lock_idx], __ATOMIC_ACQUIRE);
        if (pr_stm_is_locked(lw)) {
            if (pr_stm_get_priority(lw) != priority) {
                // Another thread holds the lock - abort
                goto abort_tx;
            }
            // We hold the lock - version must still match what we read
            if (pr_stm_get_version(lw) != g_tx.reads[i].ver) {
                goto abort_tx;
            }
        } else {
            // Lock free - version must match what we read
            if (pr_stm_get_version(lw) != g_tx.reads[i].ver) {
                goto abort_tx;
            }
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    commit_clock = gpu_global_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
    // ── WRITE-BACK (write buffered values to actual data addresses) ─
    for (int i = 0; i < g_tx.num_writes; i++) {
        void *data_addr = g_tx.writes[i].data_addr;
        uint64_t val = g_tx.writes[i].val;
        switch (g_tx.writes[i].bytes) {
            case 1: __atomic_store_n((uint8_t*)data_addr, (uint8_t)val, __ATOMIC_RELEASE); break;
            case 2: __atomic_store_n((uint16_t*)data_addr, (uint16_t)val, __ATOMIC_RELEASE); break;
            case 4: __atomic_store_n((uint32_t*)data_addr, (uint32_t)val, __ATOMIC_RELEASE); break;
            case 8: __atomic_store_n((uint64_t*)data_addr, val, __ATOMIC_RELEASE); break;
        }
    }
    // ── Release locks & update version ──────────────────────────
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; i < g_tx.num_writes; i++) {
        uint32_t lock_idx = g_tx.writes[i].lock_idx;
        uint32_t new_entry = pr_stm_make_entry(0, commit_clock, 0);
        __atomic_store_n(&gpu_lock_table[lock_idx], new_entry, __ATOMIC_RELEASE);
    }
    tm_flush_spec_allocs();
    tm_flush_deferred_frees();
    return;

abort_tx:
    for (int i = 0; i < g_tx.num_writes; i++) {
        uint32_t lock_idx = g_tx.writes[i].lock_idx;
        uint32_t lw = __atomic_load_n(&gpu_lock_table[lock_idx], __ATOMIC_RELAXED);
        if (pr_stm_is_locked(lw) && pr_stm_get_priority(lw) == priority) {
            uint32_t new_entry = pr_stm_make_entry(0, pr_stm_get_version(lw), 0);
            __atomic_store_n(&gpu_lock_table[lock_idx], new_entry, __ATOMIC_RELEASE);
        }
    }
    siglongjmp(tm_jmpbuf, 1);
}

// ── Read / Write helpers ──────────────────────────────────────────

static uint32_t* get_lock_entry(void *addr) {
    // Hash the address to a lock table index
    uint32_t idx = ((uintptr_t)addr >> 3) & (PR_STM_LOCKTABLE_SIZE - 1);
    return &gpu_lock_table[idx];
}

static int find_write(void *data_addr) {
    for (int i = g_tx.num_writes - 1; i >= 0; i--) {
        if (g_tx.writes[i].data_addr == data_addr)
            return i;
    }
    return -1;
}

static uint64_t read_common(uint32_t *lock_entry_ptr, void *data_ptr, uint8_t bytes) {
    // Check write-set first (read-own-writes)
    int wi = find_write(data_ptr);
    if (wi >= 0) {
        return g_tx.writes[wi].val;
    }
    uint32_t lw = __atomic_load_n(lock_entry_ptr, __ATOMIC_ACQUIRE);
    if (g_tx.num_reads < PR_STM_MAX_READS) {
        uint32_t idx = (uint32_t)(lock_entry_ptr - gpu_lock_table);
        g_tx.reads[g_tx.num_reads].lock_idx = idx;
        g_tx.reads[g_tx.num_reads].ver = pr_stm_get_version(lw);
        g_tx.num_reads++;
    }
    if (bytes == 8) {
        uint64_t val;
        __builtin_memcpy(&val, data_ptr, 8);
        return val;
    } else {
        return __atomic_load_n((uint32_t*)data_ptr, __ATOMIC_ACQUIRE);
    }
}

static void write_common(uint32_t *lock_entry_ptr, uint32_t *data_ptr, uint64_t val, uint8_t bytes) {
    if (g_tx.num_writes < PR_STM_MAX_WRITES) {
        uint32_t idx = (uint32_t)(lock_entry_ptr - gpu_lock_table);
        g_tx.writes[g_tx.num_writes].lock_idx = idx;
        g_tx.writes[g_tx.num_writes].data_addr = data_ptr;
        g_tx.writes[g_tx.num_writes].val = val;
        g_tx.writes[g_tx.num_writes].bytes = bytes;
        g_tx.num_writes++;
    }
}

// ── Alloc — use TM region allocator ───────────────────────────────

static void* real_tm_malloc(size_t sz)   { return stm::tm_region_malloc(sz); }
static void* real_tm_calloc(size_t n, size_t sz) { return stm::tm_region_calloc(n, sz); }
static void* real_tm_realloc(void* p, size_t sz) { return stm::tm_region_realloc(p, sz); }
static void  real_tm_free(void* p)       { stm::tm_region_free(p); }

// ── Read stubs ────────────────────────────────────────────────────

static uint8_t  real_tm_read_i1(uint8_t  *a) { return (uint8_t) read_common(get_lock_entry(a), (uint32_t*)a, 1); }
static uint16_t real_tm_read_i2(uint16_t *a) { return (uint16_t)read_common(get_lock_entry(a), (uint32_t*)a, 2); }
static uint32_t real_tm_read_i4(uint32_t *a) { return (uint32_t)read_common(get_lock_entry(a), a, 4); }
static uint64_t real_tm_read_i8(uint64_t *a) { return read_common(get_lock_entry(a), a, 8); }
static float    real_tm_read_f4(float    *a) { float v; int wi = find_write(a); if (wi >= 0) { uint32_t r = (uint32_t)g_tx.writes[wi].val; __builtin_memcpy(&v, &r, 4); return v; } uint32_t r = (uint32_t)read_common(get_lock_entry(a), (uint32_t*)a, 4); __builtin_memcpy(&v, &r, 4); return v; }
static double   real_tm_read_f8(double   *a) { int wi = find_write(a); if (wi >= 0) { double v; uint64_t r = g_tx.writes[wi].val; __builtin_memcpy(&v, &r, 8); return v; } double v; uint64_t r = read_common(get_lock_entry(a), a, 8); __builtin_memcpy(&v, &r, 8); return v; }
static void*    real_tm_read_ptr(void*    *a) { int wi = find_write(a); if (wi >= 0) { uint64_t r = g_tx.writes[wi].val; void *v; __builtin_memcpy(&v, &r, 8); return v; } void *v; uint64_t r = read_common(get_lock_entry(a), a, 8); __builtin_memcpy(&v, &r, 8); return v; }

// ── Write stubs ───────────────────────────────────────────────────

static void real_tm_write_i1(uint8_t  *a, uint8_t  v) { write_common(get_lock_entry(a), (uint32_t*)a, v, 1); }
static void real_tm_write_i2(uint16_t *a, uint16_t v) { write_common(get_lock_entry(a), (uint32_t*)a, v, 2); }
static void real_tm_write_i4(uint32_t *a, uint32_t v) { write_common(get_lock_entry(a), a, v, 4); }
static void real_tm_write_i8(uint64_t *a, int64_t v) { write_common(get_lock_entry(a), (uint32_t*)a, (uint64_t)v, 8); }
static void real_tm_write_f4(float    *a, float    v) { uint32_t r; __builtin_memcpy(&r, &v, 4); real_tm_write_i4((uint32_t*)a, r); }
static void real_tm_write_f8(double   *a, double   v) { uint64_t r; __builtin_memcpy(&r, &v, 8); real_tm_write_i8((uint64_t*)a, (int64_t)r); }
static void real_tm_write_ptr(void*    *a, void*    v) { uint64_t r; __builtin_memcpy(&r, &v, 8); real_tm_write_i8((uint64_t*)a, (int64_t)r); }

static void* real_tm_get_env()          { return nullptr; }
static void  real_tm_set_jmpbuf(void*)  {}
static void* real_tm_get_thread_state() { return nullptr; }

// ── Hook table (order matches TMRealHooks struct) ────────────────

static constexpr TMRealHooks g_gpu_stm_hooks = {
    .begin    = real_tm_begin,
    .end      = real_tm_end,
    .malloc   = real_tm_malloc,
    .calloc   = real_tm_calloc,
    .realloc  = real_tm_realloc,
    .free     = real_tm_free,
    .read_i1  = real_tm_read_i1,
    .read_i2  = real_tm_read_i2,
    .read_i4  = real_tm_read_i4,
    .read_i8  = real_tm_read_i8,
    .read_f4  = real_tm_read_f4,
    .read_f8  = real_tm_read_f8,
    .read_ptr = real_tm_read_ptr,
    .write_i1 = real_tm_write_i1,
    .write_i2 = real_tm_write_i2,
    .write_i4 = real_tm_write_i4,
    .write_i8 = real_tm_write_i8,
    .write_f4 = real_tm_write_f4,
    .write_f8 = real_tm_write_f8,
    .write_ptr = real_tm_write_ptr,
    .get_env  = real_tm_get_env,
    .set_jmpbuf = real_tm_set_jmpbuf,
    .get_thread_state = real_tm_get_thread_state,
};

// ── Lifecycle (TEXT functions, matching tm_api.hpp declaration) ────
// tm_api.hpp declares tm_init/tm_exit/tm_init_thread/tm_exit_thread
// as plain TEXT functions (lines 17-20).  We match that here for the
// non-plugin case.  For LLVM_TM_PLUGIN, they become DATA variables
// (function pointers) — see the STAMP fix in AGENTS.md.

static void do_tm_init();
static void do_tm_exit();
static void do_tm_init_thread();
static void do_tm_exit_thread();

#ifdef LLVM_TM_PLUGIN
void (*tm_init)()        = do_tm_init;
void (*tm_exit)()        = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;
static void do_tm_init()
#else
extern "C" void tm_init()
#endif
{
    stm::tm_region_init();
    gpu_lock_table = (uint32_t*)std::calloc(PR_STM_LOCKTABLE_SIZE, sizeof(uint32_t));
    if (!gpu_lock_table) {
        fprintf(stderr, "FATAL: gpu_stm_cpu: calloc lock table failed\n");
        std::abort();
    }
    gpu_global_clock.store(0, std::memory_order_relaxed);
    tm_register_real_hooks(&g_gpu_stm_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
extern "C" void tm_exit()
#endif
{
    std::free(gpu_lock_table);
    gpu_lock_table = nullptr;
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
