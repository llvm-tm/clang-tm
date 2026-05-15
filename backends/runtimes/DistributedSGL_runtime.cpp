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
 *      Only one process holds the lock at a time.
 *   4. tm_end releases the lock and msyncs (COMMIT phase).
 *   5. tm_exit decrements the process count.
 *
 * The shared state (lock, counters, TM symbol data) is in a file
 * that maps to a git-ignored directory.
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
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

// ── Shared state in mmap ────────────────────────────────────────
// All fields are volatile + plain integers (not std::atomic) so the
// struct is standard-layout and safe for placement in shared memory.

struct SharedState {
    volatile int   ready_count;       // how many processes have called init
    volatile int   process_count;     // total N (set by first process)
    volatile uint8_t lock;            // spinlock: 0=free, 1=held
    uint8_t        _pad1[7];
    volatile int64_t epoch;           // incremented on each commit
};

static const char* SHM_FILE = "benchmark_results/tm_2pc_state.bin";
static int g_shm_fd = -1;

static SharedState* g_state = nullptr;     // pointer into mmap
static uint8_t*     g_data_base = nullptr;  // TM symbol data in mmap
static size_t       g_mmap_size = 0;
static size_t       g_data_off = 0;         // offset where TM data starts

extern "C" {
extern uint32_t tm_symbol_count;
extern const char* tm_symbol_names[];
extern void* tm_symbol_addresses[];
extern uint64_t tm_symbol_sizes[];

// ── Spinlock helpers (shared-memory safe, portable) ────────────

static void spin_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

static void spin_lock(volatile uint8_t* lk) {
    while (__sync_lock_test_and_set(lk, 1))
        while (*lk) spin_pause();
}

static void spin_unlock(volatile uint8_t* lk) {
    __sync_lock_release(lk);
}

// ── tm_init ─────────────────────────────────────────────────────

void tm_init() {
    // Compute total data size
    uint64_t data_size = 0;
    for (uint32_t i = 0; i < tm_symbol_count; i++)
        data_size += tm_symbol_sizes[i];

    g_data_off = sizeof(SharedState);
    g_mmap_size = g_data_off + data_size;

    // Open / create the shared file (in git-ignored benchmark_results/)
    g_shm_fd = open(SHM_FILE, O_RDWR | O_CREAT, 0644);
    if (g_shm_fd < 0) { perror("open"); exit(1); }

    if (ftruncate(g_shm_fd, g_mmap_size) < 0) { perror("ftruncate"); exit(1); }

    void* base = mmap(nullptr, g_mmap_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, g_shm_fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    close(g_shm_fd);

    g_state = static_cast<SharedState*>(base);
    g_data_base = static_cast<uint8_t*>(base) + g_data_off;

    // First process to init initialises the shared state
    int expected = 0;
    int nproc = 1;
    char* env = getenv("TM_NPROCESSES");
    if (env) nproc = atoi(env);
    if (nproc < 1) nproc = 1;

    // Atomically check-and-set process_count (first process wins)
    if (__sync_bool_compare_and_swap(&g_state->process_count, 0, nproc)) {
        g_state->ready_count = 0;
        g_state->lock = 0;
        g_state->epoch = 0;
        // Snapshot initial TM symbol data
        uint64_t off = 0;
        for (uint32_t i = 0; i < tm_symbol_count; i++) {
            uint64_t sz = tm_symbol_sizes[i];
            memcpy(g_data_base + off, tm_symbol_addresses[i], sz);
            off += sz;
        }
        fprintf(stderr, "[DistSGL] init: process_count=%d\n", nproc);
    } else {
        // Restore TM symbol data from shared state
        uint64_t off = 0;
        for (uint32_t i = 0; i < tm_symbol_count; i++) {
            uint64_t sz = tm_symbol_sizes[i];
            memcpy(tm_symbol_addresses[i], g_data_base + off, sz);
            off += sz;
        }
    }

    // Barrier: wait until all processes have inited
    __sync_fetch_and_add(&g_state->ready_count, 1);
    fprintf(stderr, "[DistSGL] barrier: %d/%d ready\n",
            g_state->ready_count, g_state->process_count);
    while (g_state->ready_count < g_state->process_count) {
        usleep(1000);  // 1 ms
    }
    fprintf(stderr, "[DistSGL] all %d processes ready, starting\n", nproc);
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
    __sync_fetch_and_sub(&g_state->ready_count, 1);
    if (g_state->ready_count <= 0) {
        // Last process cleans up
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
                getpid(), (long long)g_state->epoch);
        spin_lock(&g_state->lock);
    }
}

void tm_end() {
    if (tm_nested_call_counter == 1) {
        // COMMIT phase: msync, increment epoch, release lock
        msync(g_state, g_mmap_size, MS_SYNC);
        __sync_fetch_and_add(&g_state->epoch, 1);
        fprintf(stderr, "[DistSGL] P%d COMMIT  epoch=%lld\n",
                getpid(), (long long)g_state->epoch);
        spin_unlock(&g_state->lock);
    }
}

// ── Read/write hooks ───────────────────────────────────────────

void   tm_load_symbols(void*, uint32_t) {}

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
