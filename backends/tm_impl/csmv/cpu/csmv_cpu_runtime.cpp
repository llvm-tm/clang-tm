#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_set>
#include <algorithm>

#include "csmv_api.h"
#include "tm_hooks.hpp"
#include "tm_thread_state.hpp"
#include "tm_alloc_overrides.hpp"

// ── TLS variables required by tm_alloc_overrides.hpp ──────────────
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
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

// ── Per-thread CSMV transaction state ────────────────────────────

static thread_local CSMVThreadTx g_tx;

// GC: version nodes older than low_water_mark can be reclaimed.
// We maintain a per-thread free list of reclaimed nodes.

struct GCNode { CSMVVersionNode *node; GCNode *next; };
static thread_local GCNode *g_gc_list = nullptr;

static void gc_reclaim_node(CSMVVersionNode *node) {
    GCNode *gc = (GCNode*)std::malloc(sizeof(GCNode));
    gc->node = node;
    gc->next = g_gc_list;
    g_gc_list = gc;
}

static void gc_flush() {
    while (g_gc_list) {
        GCNode *tmp = g_gc_list;
        g_gc_list = g_gc_list->next;
        std::free(tmp->node);
        std::free(tmp);
    }
}

// ── Hash helpers ─────────────────────────────────────────────────

static CSMVObjectEntry* csmv_get_entry(void *addr) {
    uint64_t idx = ((uintptr_t)addr >> 3) & (CSMV_TABLE_SIZE - 1);
    return &g_csmv_table[idx];
}

// ── begin / end ──────────────────────────────────────────────────

static void real_tm_begin() {
    g_tx.start_clock = g_csmv_clock.load(std::memory_order_acquire);
    g_tx.num_reads = 0;
    g_tx.num_writes = 0;
    g_csmv_active_txns.fetch_add(1, std::memory_order_relaxed);
    tm_clear_spec_allocs();
    tm_clear_deferred_frees();
}

static void real_tm_end() {
    uint64_t commit_ts = 0;

    // ── Lock all write-set entries (in address order) ─────────
    // First sort by entry pointer to prevent deadlock
    if (g_tx.num_writes > 1) {
        std::sort(g_tx.writes, g_tx.writes + g_tx.num_writes,
            [](const auto &a, const auto &b) { return a.entry < b.entry; });
    }

    // Lock — skip duplicates (same entry, different addresses hashing to same slot)
    for (int i = 0; i < g_tx.num_writes; i++) {
        if (i == 0 || g_tx.writes[i].entry != g_tx.writes[i-1].entry)
            g_tx.writes[i].entry->lock.lock();
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
            node = (CSMVVersionNode*)std::malloc(sizeof(CSMVVersionNode));
            g_tx.writes[i].node = node;
        }
        node->timestamp = commit_ts;
        node->value = val;
        node->next = old_head;

        entry->head.store(node, std::memory_order_release);

        // Write-back to actual data address for non-TM readers (peek())
        switch (bytes) {
            case 1: __atomic_store_n((uint8_t*)data_addr, (uint8_t)val, __ATOMIC_RELEASE); break;
            case 2: __atomic_store_n((uint16_t*)data_addr, (uint16_t)val, __ATOMIC_RELEASE); break;
            case 4: __atomic_store_n((uint32_t*)data_addr, (uint32_t)val, __ATOMIC_RELEASE); break;
            case 8: __atomic_store_n((uint64_t*)data_addr, val, __ATOMIC_RELEASE); break;
        }
    }

    // ── Unlock ───────────────────────────────────────────────
    for (int i = g_tx.num_writes - 1; i >= 0; i--) {
        if (i == 0 || g_tx.writes[i].entry != g_tx.writes[i-1].entry)
            g_tx.writes[i].entry->lock.unlock();
    }

    g_tx.commit_count++;
    g_csmv_active_txns.fetch_sub(1, std::memory_order_relaxed);

    // ── Opportunistic GC every CSMV_GC_INTERVAL commits ──────
    if (g_tx.commit_count % CSMV_GC_INTERVAL == 0) {
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
                if (!head) continue;
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
    // Release any held locks
    for (int i = g_tx.num_writes - 1; i >= 0; i--) {
        if (i == 0 || g_tx.writes[i].entry != g_tx.writes[i-1].entry)
            g_tx.writes[i].entry->lock.unlock();
    }
    g_csmv_active_txns.fetch_sub(1, std::memory_order_relaxed);
    siglongjmp(tm_jmpbuf, 1);
}

// ── Read helpers ─────────────────────────────────────────────────
// CSMV read: find the newest version with timestamp ≤ start_clock.
// This NEVER aborts — multi-versioning guarantees a consistent view.

static int find_write(void *data_addr) {
    for (int i = g_tx.num_writes - 1; i >= 0; i--) {
        if (g_tx.writes[i].data_addr == data_addr)
            return i;
    }
    return -1;
}

static uint64_t read_common(void *data_addr) {
    // Check write-set first (read-own-writes)
    int wi = find_write(data_addr);
    if (wi >= 0)
        return g_tx.writes[wi].val;

    CSMVObjectEntry *entry = csmv_get_entry(data_addr);

    // Traverse version list to find newest version ≤ start_clock
    CSMVVersionNode *head = entry->head.load(std::memory_order_consume);
    uint64_t result;

    if (head == nullptr) {
        // No versions yet — return raw value (initial state before any write)
        // This supports non-TM initialization (e.g., TM<int> x(10))
        result = *(uint64_t*)data_addr;
    } else if (head->timestamp <= g_tx.start_clock) {
        // Head is the newest version and within our snapshot → fast path
        result = head->value;
    } else {
        // Head is too new — traverse to find the right version
        CSMVVersionNode *node = head;
        result = 0;
        while (node) {
            if (node->timestamp <= g_tx.start_clock) {
                result = node->value;
                break;
            }
            node = node->next;
        }
    }

    // Record read for validation (record the head timestamp)
    if (g_tx.num_reads < CSMV_MAX_READS) {
        g_tx.reads[g_tx.num_reads].entry = entry;
        g_tx.reads[g_tx.num_reads].observed_ts = head ? head->timestamp : 0;
        g_tx.num_reads++;
    }

    return result;
}

// ── Write helper ─────────────────────────────────────────────────

static void write_common(void *data_addr, uint64_t val, uint8_t bytes) {
    CSMVObjectEntry *entry = csmv_get_entry(data_addr);

    // Check if we already have a write to this entry (same hash bucket)
    // Use data_addr for matching, not entry pointer (different addresses may map
    // to the same entry — each has its own write entry)
    for (int i = 0; i < g_tx.num_writes; i++) {
        if (g_tx.writes[i].entry == entry && g_tx.writes[i].data_addr == data_addr) {
            g_tx.writes[i].val = val;
            g_tx.writes[i].bytes = bytes;
            return;
        }
    }

    // Add new write entry
    if (g_tx.num_writes < CSMV_MAX_WRITES) {
        g_tx.writes[g_tx.num_writes].entry = entry;
        g_tx.writes[g_tx.num_writes].data_addr = data_addr;
        g_tx.writes[g_tx.num_writes].node = nullptr;
        g_tx.writes[g_tx.num_writes].val = val;
        g_tx.writes[g_tx.num_writes].bytes = bytes;
        g_tx.num_writes++;
    }
}

// ── Alloc — use TM region allocator ───────────────────────────────

static void* real_tm_malloc(size_t sz)    { return stm::tm_region_malloc(sz); }
static void* real_tm_calloc(size_t n, size_t sz) { return stm::tm_region_calloc(n, sz); }
static void* real_tm_realloc(void* p, size_t sz) { return stm::tm_region_realloc(p, sz); }
static void  real_tm_free(void* p)        { stm::tm_region_free(p); }

// ── Read stubs ───────────────────────────────────────────────────

static uint8_t  real_tm_read_i1(uint8_t  *a) { return (uint8_t) read_common(a); }
static uint16_t real_tm_read_i2(uint16_t *a) { return (uint16_t)read_common(a); }
static uint32_t real_tm_read_i4(uint32_t *a) { return (uint32_t)read_common(a); }
static uint64_t real_tm_read_i8(uint64_t *a) { return read_common(a); }
static float    real_tm_read_f4(float    *a) { uint64_t r = read_common(a); float v; __builtin_memcpy(&v, &r, 4); return v; }
static double   real_tm_read_f8(double   *a) { uint64_t r = read_common(a); double v; __builtin_memcpy(&v, &r, 8); return v; }
static void*    real_tm_read_ptr(void*    *a) { uint64_t r = read_common(a); void *v; __builtin_memcpy(&v, &r, 8); return v; }

// ── Write stubs ──────────────────────────────────────────────────

static void real_tm_write_i1(uint8_t  *a, uint8_t  v) { write_common(a, v, 1); }
static void real_tm_write_i2(uint16_t *a, uint16_t v) { write_common(a, v, 2); }
static void real_tm_write_i4(uint32_t *a, uint32_t v) { write_common(a, v, 4); }
static void real_tm_write_i8(uint64_t *a, int64_t v)  { write_common(a, (uint64_t)v, 8); }
static void real_tm_write_f4(float    *a, float    v) { uint64_t r; __builtin_memcpy(&r, &v, 4); write_common(a, r, 4); }
static void real_tm_write_f8(double   *a, double   v) { uint64_t r; __builtin_memcpy(&r, &v, 8); write_common(a, r, 8); }
static void real_tm_write_ptr(void*    *a, void*    v) { uint64_t r; __builtin_memcpy(&r, &v, 8); write_common(a, r, 8); }

static void* real_tm_get_env()          { return nullptr; }
static void  real_tm_set_jmpbuf(void*)  {}
static void* real_tm_get_thread_state() { return nullptr; }

// ── Hook table ───────────────────────────────────────────────────

static constexpr TMRealHooks g_csmv_hooks = {
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

// ── Lifecycle ────────────────────────────────────────────────────

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

    // Allocate and zero-initialize object table
    g_csmv_table = (CSMVObjectEntry*)std::calloc(CSMV_TABLE_SIZE, sizeof(CSMVObjectEntry));
    if (!g_csmv_table) {
        fprintf(stderr, "FATAL: csmv_cpu: calloc object table failed\n");
        std::abort();
    }

    // Initialize each entry's head to nullptr
    for (uint64_t i = 0; i < CSMV_TABLE_SIZE; i++) {
        new (&g_csmv_table[i].lock) std::mutex();
        g_csmv_table[i].head.store(nullptr, std::memory_order_relaxed);
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
