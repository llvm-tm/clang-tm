/**
 * DistributedSGL — 2-Phase Commit Backend over Shared Mmap
 *
 * Simulates a distributed transaction system using a single global
 * lock and two-phase commit over a shared mmap file.
 *
 * Protocol:
 *   1. Each process sets TM_NPROCESSES=N (env var).
 *   2. tm_init waits until N processes have called tm_init (barrier).
 *   3. tm_begin acquires a global spinlock (PREPARE phase).
 *   4. tm_end releases the lock and msyncs (COMMIT phase).
 *   5. tm_exit decrements the process count.
 *
 * All atomic operations use std::atomic<int> with sequential consistency.
 * The shared state is in a file inside benchmark_results/ (git-ignored).
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
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

// ── Shared state in mmap ────────────────────────────────────────
// Uses std::atomic for cross-process synchronization.
// std::atomic<int> and std::atomic<uint8_t> are standard-layout on
// all major implementations and safe in MAP_SHARED memory when both
// processes use the same C++ standard library.

struct SharedState {
    std::atomic<int>      ready_count {0};
    std::atomic<int>      process_count {0};
    std::atomic<uint8_t>  lock_flag {0};    // spinlock: 0=free, 1=held
    uint8_t               _pad[7];
    std::atomic<int64_t>  epoch {0};
};
static_assert(sizeof(SharedState) == 24, "SharedState unexpected size");

static const char* SHM_FILE = "benchmark_results/tm_2pc_state.bin";
static SharedState* g_state = nullptr;
static uint8_t*     g_data_base = nullptr;
static size_t       g_mmap_size = 0;
static size_t       g_data_off = 0;

extern "C" {
extern uint32_t tm_symbol_count;
extern const char* tm_symbol_names[];
extern void* tm_symbol_addresses[];
extern uint64_t tm_symbol_sizes[];

// ── Spinlock (shared-memory safe) ───────────────────────────────
// Uses std::atomic<uint8_t> with sequential consistency.
// No arch-specific pause instructions — uses yield.

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

    // Read TM_NPROCESSES from environment
    int nproc = 1;
    if (char* env = getenv("TM_NPROCESSES")) {
        nproc = atoi(env);
        if (nproc < 1) nproc = 1;
    }

    // First process wins: CAS process_count from 0 to nproc
    int expected = 0;
    if (g_state->process_count.compare_exchange_strong(expected, nproc,
            std::memory_order_acq_rel)) {
        // First process: snapshot initial TM data into the shared file
        g_state->ready_count.store(0, std::memory_order_relaxed);
        uint64_t off = 0;
        for (uint32_t i = 0; i < tm_symbol_count; i++) {
            uint64_t sz = tm_symbol_sizes[i];
            memcpy(g_data_base + off, tm_symbol_addresses[i], sz);
            off += sz;
        }
        fprintf(stderr, "[DistSGL] init: process_count=%d\n", nproc);
    } else {
        // Subsequent processes: restore TM data from shared state
        uint64_t off = 0;
        for (uint32_t i = 0; i < tm_symbol_count; i++) {
            uint64_t sz = tm_symbol_sizes[i];
            memcpy(tm_symbol_addresses[i], g_data_base + off, sz);
            off += sz;
        }
    }

    // Barrier: signal ready and wait
    g_state->ready_count.fetch_add(1, std::memory_order_acq_rel);
    fprintf(stderr, "[DistSGL] barrier: %d/%d ready\n",
            g_state->ready_count.load(std::memory_order_acquire),
            g_state->process_count.load(std::memory_order_acquire));
    while (g_state->ready_count.load(std::memory_order_acquire) <
           g_state->process_count.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fprintf(stderr, "[DistSGL] all ready, starting\n");
}

void tm_exit() {
    if (g_data_base && g_state) {
        // Snapshot TM data back to shared state
        uint64_t off = 0;
        for (uint32_t i = 0; i < tm_symbol_count; i++) {
            uint64_t sz = tm_symbol_sizes[i];
            memcpy(g_data_base + off, tm_symbol_addresses[i], sz);
            off += sz;
        }
        msync(g_state, g_mmap_size, MS_SYNC);
    }
    g_state->ready_count.fetch_sub(1, std::memory_order_acq_rel);
    if (g_state->ready_count.load(std::memory_order_acquire) <= 0) {
        munmap(g_state, g_mmap_size);
        unlink(SHM_FILE);
    }
    fprintf(stderr, "[DistSGL] exit\n");
}

// ── Transaction boundaries (2PC) ───────────────────────────────

__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
__thread sigjmp_buf tm_jmpbuf;

int tm_setjmp() { return 0; }
void tm_set_jmpbuf(void*) {}
sigjmp_buf* tm_get_env() { return &tm_jmpbuf; }
void tm_set_env(sigjmp_buf* env) {
    if (env) memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
}

void tm_begin() {
    if (tm_nested_call_counter == 1) {
        // PREPARE phase: acquire global lock
        fprintf(stderr, "[DistSGL] P%d PREPARE epoch=%lld\n",
                getpid(), (long long)g_state->epoch.load(std::memory_order_relaxed));
        spin_lock(&g_state->lock_flag);
    }
}

void tm_end() {
    if (tm_nested_call_counter == 1) {
        // COMMIT phase: msync, increment epoch, release lock
        msync(g_state, g_mmap_size, MS_SYNC);
        g_state->epoch.fetch_add(1, std::memory_order_release);
        fprintf(stderr, "[DistSGL] P%d COMMIT  epoch=%lld\n",
                getpid(), (long long)g_state->epoch.load(std::memory_order_relaxed));
        spin_unlock(&g_state->lock_flag);
    }
}

// ── Read/write hooks ───────────────────────────────────────────

void tm_load_symbols(void*, uint32_t) {}

uint8_t  tm_read_i1(volatile uint8_t*  a) { return *a; }
uint16_t tm_read_i2(volatile uint16_t* a) { return *a; }
uint32_t tm_read_i4(volatile uint32_t* a) { return *a; }
uint64_t tm_read_i8(volatile uint64_t* a) { return *a; }
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

} // extern "C"
