/**
 * DistributedSGL — 2-Phase Commit Backend over Shared Mmap
 *
 * Simulates a distributed transaction system using a single global
 * lock and two-phase commit over a shared mmap file.
 *
 * Protocol:
 *   1. Each process sets TM_NPROCESSES=N (env var).
 *   2. tm_init waits until N processes have called tm_init (barrier).
 *   3. tm_init sets RelPtr base so offset_ptr works in shared data.
 *   4. tm_begin acquires the global spinlock (PREPARE), then syncs
 *      TM symbol data FROM the shared mmap (read latest committed state).
 *   5. tm_end syncs TM symbol data TO the shared mmap, msyncs,
 *      increments epoch, then releases the lock (COMMIT).
 *   6. tm_exit decrements the process count.
 *
 * On every tm_begin → tm_end pair, ALL TM-annotated globals are
 * copied between process-local memory and the shared mmap.  This
 * ensures each transaction sees the latest committed state from any
 * process, and publishes its own changes atomically.
 *
 * All atomic operations use std::atomic with sequential consistency.
 * The shared state is in benchmark_results/ (git-ignored).
 *
 * Usage:
 *   export TM_NPROCESSES=2
 *   ./bin/prog &   # process 1
 *   ./bin/prog     # process 2
 */

#include <cstdint>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include "tm_alloc_overrides.hpp"
thread_local bool g_in_tx = false;
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "rel_ptr.hpp"

// ── Shared state in mmap ────────────────────────────────────────

struct SharedState {
    std::atomic<int>      ready_count {0};
    std::atomic<int>      process_count {0};
    std::atomic<uint8_t>  lock_flag {0};
    uint8_t               _pad[7];
    std::atomic<int64_t>  epoch {0};
};
static_assert(sizeof(SharedState) == 24, "SharedState unexpected size");

static constexpr const char* SHM_FILE = "benchmark_results/tm_2pc_state.bin";
static SharedState* g_state = nullptr;
static uint8_t*     g_data_base = nullptr;   // TM data in mmap
static size_t       g_mmap_size = 0;
static size_t       g_data_off = 0;

extern "C" {
extern uint32_t tm_symbol_count;
extern const char* tm_symbol_names[];
extern void* tm_symbol_addresses[];
extern uint64_t tm_symbol_sizes[];

// ── Spinlock ────────────────────────────────────────────────────

static void spin_lock(std::atomic<uint8_t>* lk) {
    uint8_t expected = 0;
    while (!lk->compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        expected = 0;
        std::this_thread::yield();
    }
}

static void spin_unlock(std::atomic<uint8_t>* lk) {
    lk->store(0, std::memory_order_release);
}

// ── Data transfer helpers ───────────────────────────────────────
// Copy TM symbol data between process-local addresses and shared mmap.

static void sync_local_to_shared() {
    uint64_t off = 0;
    for (uint32_t i = 0; i < tm_symbol_count; i++) {
        uint64_t sz = tm_symbol_sizes[i];
        memcpy(g_data_base + off, tm_symbol_addresses[i], sz);
        off += sz;
    }
}

static void sync_shared_to_local() {
    uint64_t off = 0;
    for (uint32_t i = 0; i < tm_symbol_count; i++) {
        uint64_t sz = tm_symbol_sizes[i];
        memcpy(tm_symbol_addresses[i], g_data_base + off, sz);
        off += sz;
    }
}

// ── tm_init ─────────────────────────────────────────────────────

void tm_init() {
    uint64_t data_size = 0;
    for (uint32_t i = 0; i < tm_symbol_count; i++)
        data_size += tm_symbol_sizes[i];

    g_data_off = sizeof(SharedState);
    g_mmap_size = g_data_off + data_size;

    int fd = open(SHM_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); exit(1); }
    if (ftruncate(fd, g_mmap_size) < 0) { perror("ftruncate"); exit(1); }

    void* base = mmap(nullptr, g_mmap_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);

    g_state = static_cast<SharedState*>(base);
    g_data_base = static_cast<uint8_t*>(base) + g_data_off;

    // Set the RelPtr base so offset_ptr works in shared data
    RelPtr<void>::set_base(base);

    int nproc = 1;
    if (char* env = getenv("TM_NPROCESSES")) {
        nproc = atoi(env);
        if (nproc < 1) nproc = 1;
    }

    int expected = 0;
    if (g_state->process_count.compare_exchange_strong(expected, nproc,
            std::memory_order_acq_rel)) {
        // First process: snapshot initial TM data into shared file
        g_state->ready_count.store(0, std::memory_order_relaxed);
        sync_local_to_shared();
    } else {
        // Subsequent processes: load TM data from shared state
        sync_shared_to_local();
    }

    // Barrier
    g_state->ready_count.fetch_add(1, std::memory_order_acq_rel);
    while (g_state->ready_count.load(std::memory_order_acquire) <
           g_state->process_count.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void tm_exit() {
    if (g_data_base && g_state) {
        sync_local_to_shared();
        msync(g_state, g_mmap_size, MS_ASYNC);
    }
    g_state->ready_count.fetch_sub(1, std::memory_order_acq_rel);
    if (g_state->ready_count.load(std::memory_order_acquire) <= 0) {
        munmap(g_state, g_mmap_size);
        unlink(SHM_FILE);
    }
}

// ── Transaction boundaries (2PC with shared-memory sync) ────────

__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
__thread sigjmp_buf tm_jmpbuf;

int tm_setjmp() { return 0; }
void tm_set_jmpbuf(void*) {}
sigjmp_buf* tm_get_env() { return &tm_jmpbuf; }
void tm_set_env(sigjmp_buf* env) {
    if (env) memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
}

// Per-thread: true until the first tm_begin publishes local state to mmap
static thread_local bool g_first_begin = true;

void tm_begin() {
    g_in_tx = true;
    // First transaction in this thread: publish local init state
    // (benchmarks initialize TM globals AFTER tm_init, so the mmap
    //  has stale data until we publish here).
    if (g_first_begin) {
        sync_local_to_shared();
        g_first_begin = false;
    }

    // PREPARE phase: acquire global lock, then read latest state
    spin_lock(&g_state->lock_flag);
    sync_shared_to_local();
}

void tm_end() {
    g_in_tx = false;
    // COMMIT phase: publish state, advance epoch, release lock
    sync_local_to_shared();
    g_state->epoch.fetch_add(1, std::memory_order_release);
    spin_unlock(&g_state->lock_flag);
}

// ── Read/write hooks (direct memory, no instrumentation beyond serialization) ──

void tm_load_symbols(void*, uint32_t) {}

uint8_t  tm_read_i1(volatile uint8_t*  a) { return *a; }
uint16_t tm_read_i2(volatile uint16_t* a) { return *a; }
uint32_t tm_read_i4(volatile uint32_t* a) { return *a; }
uint64_t tm_read_i8(volatile uint64_t* a) { return *a; }
void tm_read_i16(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    out_words[0] = vaddr[0];
    out_words[1] = vaddr[1];
}
void tm_read_i32(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 4; i++) out_words[i] = vaddr[i];
}
void tm_read_i64(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 8; i++) out_words[i] = vaddr[i];
}
float    tm_read_f4(volatile float*    a) { return *a; }
double   tm_read_f8(volatile double*   a) { return *a; }
void*    tm_read_ptr(volatile void**   a) { return (void*)*a; }

void* tm_read_z(volatile uint8_t* src, uint64_t len) {
    void* buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

void tm_write_i1(volatile uint8_t*  a, uint8_t  v) { *a = v; }
void tm_write_i2(volatile uint16_t* a, uint16_t v) { *a = v; }
void tm_write_i4(volatile uint32_t* a, uint32_t v) { *a = v; }
void tm_write_i8(volatile uint64_t* a, uint64_t v) { *a = v; }
void tm_write_i16(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 2; i++) vaddr[i] = val_words[i];
}
void tm_write_i32(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 4; i++) vaddr[i] = val_words[i];
}
void tm_write_i64(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 8; i++) vaddr[i] = val_words[i];
}
void tm_write_f4(volatile float*    a, float    v) { *a = v; }
void tm_write_f8(volatile double*   a, double   v) { *a = v; }
void tm_write_ptr(volatile void**   a, void*    v) { *a = v; }

void tm_write_z(volatile uint8_t* d, volatile uint8_t* s, uint64_t l) {
    memcpy((void*)d, (const void*)s, l);
}
void tm_memset(volatile uint8_t* a, uint8_t v, uint64_t l) {
    memset((void*)a, v, l);
}

// ── Stubs ───────────────────────────────────────────────────────

void tm_init_thread() {}
void tm_exit_thread() {}

static std::recursive_mutex g_serialize_mutex;
void tm_serialize_lock()   { g_serialize_mutex.lock(); }
void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

void consume_ptr(volatile void*) {}

// TM allocator stubs (redirect to system allocator)
void* tm_malloc(size_t size) { return g_in_tx ? malloc(size) : malloc(size); }
void* tm_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void* tm_realloc(void* ptr, size_t size) { return realloc(ptr, size); }
void  tm_free(void* ptr) { free(ptr); }

} // extern "C"
