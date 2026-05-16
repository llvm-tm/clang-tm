/**
 * TSX+SGL Runtime for LLVM TM Plugin
 *
 * Standard TSX lock-elision pattern (Intel OPT-GUIDE ch. 12):
 *   1. _xbegin() and, if successful, read the "lock" (sgl_epoch).
 *      If the lock is held (epoch odd), _xabort() immediately.
 *   2. The lock cache-line in the TSX read set ensures that any
 *      concurrent SGL entry (which writes to the epoch) aborts the TSX.
 *   3. After an explicit lock-busy abort, spin-wait for the lock to
 *      become free before retrying (anti-lemming effect).
 *   4. At tm_end() the epoch is re-read; if it changed an SGL
 *      transaction interleaved and we abort + fall back.
 *   5. When all retries are exhausted we acquire global_tx_lock (SGL)
 *      and write to the epoch so concurrent TSX transactions abort.
 *
 * Build with:  clang-tm -mrtm --runtime TSXSGL_runtime.cpp ...
 *
 * Hardware requirement: Intel Haswell or newer (4th gen), or any x86
 * CPU with TSX-NI (RTM).  Check /proc/cpuinfo for "rtm" flag.
 */

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <csetjmp>

#if defined(__x86_64__) || defined(__i386__)
  #include <immintrin.h>
#endif

thread_local bool g_in_tx = false;
thread_local bool in_tsx = false;
thread_local uint64_t tsx_start_epoch = 0;

static std::mutex global_tx_lock;
static std::atomic<uint64_t> sgl_epoch{0};

// Plugin-required thread-local state
__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;

enum { EPOCH_LOCK_BUSY = 0xFF, EPOCH_CHANGED = 0x01 };

// Stats counters (global, not per-thread)
static std::atomic<uint64_t> stat_tsx_started{0};
static std::atomic<uint64_t> stat_tsx_committed{0};
static std::atomic<uint64_t> stat_tsx_aborted{0};
static std::atomic<uint64_t> stat_tsx_lock_busy{0};
static std::atomic<uint64_t> stat_tsx_epoch_changed{0};
static std::atomic<uint64_t> stat_tsx_other_abort{0};
static std::atomic<uint64_t> stat_sgl_entries{0};
static std::atomic<uint64_t> stat_attempts_gt_1{0};

extern "C" {

void tm_init() {}
void tm_exit() {
    uint64_t started  = stat_tsx_started.load();
    uint64_t committed = stat_tsx_committed.load();
    uint64_t aborted   = stat_tsx_aborted.load();
    uint64_t lock_busy = stat_tsx_lock_busy.load();
    uint64_t epoch_chg = stat_tsx_epoch_changed.load();
    uint64_t other_ab  = stat_tsx_other_abort.load();
    uint64_t sgl       = stat_sgl_entries.load();
    uint64_t att_gt_1  = stat_attempts_gt_1.load();
    fprintf(stderr,
        "TSXSTATS started=%lu committed=%lu aborted=%lu "
        "lock_busy=%lu epoch_changed=%lu other_abort=%lu "
        "sgl=%lu attempts_gt_1=%lu\n",
        started, committed, aborted, lock_busy, epoch_chg, other_ab, sgl, att_gt_1);
}
void tm_init_thread() { tm_nested_call_counter = 0; in_tsx = false; }
void tm_exit_thread() {}

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }
void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp() { return 0; }
void tm_set_jmpbuf(void *) { }
sigjmp_buf* tm_get_env() { return (sigjmp_buf*)&tm_jmpbuf; }
void tm_set_env(sigjmp_buf* env) {
    if (env) memcpy(&tm_jmpbuf, env, sizeof(tm_jmpbuf));
}

void tm_load_symbols(void *, uint32_t) {}

void tm_begin() {
    if (tm_nested_call_counter > 1) return;
    g_in_tx = true;

#if defined(__x86_64__) || defined(__i386__)
    for (int attempts = 0; attempts < 5; attempts++) {
        unsigned status = _xbegin();
        if (status == _XBEGIN_STARTED) {
            stat_tsx_started.fetch_add(1, std::memory_order_relaxed);
            if (attempts > 0)
                stat_attempts_gt_1.fetch_add(1, std::memory_order_relaxed);
            // Read epoch into read set AND check if SGL is active.
            uint64_t e = sgl_epoch.load(std::memory_order_seq_cst);
            if (e & 1) {
                _xabort(EPOCH_LOCK_BUSY);
            }
            tsx_start_epoch = e;
            in_tsx = true;
            return;
        }

        stat_tsx_aborted.fetch_add(1, std::memory_order_relaxed);
        if ((status & _XABORT_EXPLICIT) &&
            _XABORT_CODE(status) == EPOCH_LOCK_BUSY) {
            stat_tsx_lock_busy.fetch_add(1, std::memory_order_relaxed);
            // Lock was busy — wait for it to become free before retrying
            // to avoid the lemming effect (thundering herd into SGL).
            while (sgl_epoch.load(std::memory_order_relaxed) & 1)
                _mm_pause();
        } else if (!(status & _XABORT_RETRY)) {
            stat_tsx_other_abort.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
#else
    (void)in_tsx;
#endif
    // Fallback: enter SGL (write to epoch so TSX threads abort).
    stat_sgl_entries.fetch_add(1, std::memory_order_relaxed);
    global_tx_lock.lock();
    sgl_epoch.fetch_add(1, std::memory_order_seq_cst);
    in_tsx = false;
}

void tm_end() {
    if (tm_nested_call_counter > 1) return;
    g_in_tx = false;

    if (in_tsx) {
#if defined(__x86_64__) || defined(__i386__)
        if (sgl_epoch.load(std::memory_order_seq_cst) != tsx_start_epoch) {
            stat_tsx_epoch_changed.fetch_add(1, std::memory_order_relaxed);
            _xabort(EPOCH_CHANGED);
        }
        stat_tsx_committed.fetch_add(1, std::memory_order_relaxed);
        _xend();
#endif
        return;
    }
    sgl_epoch.fetch_add(1, std::memory_order_seq_cst);
    global_tx_lock.unlock();
}

// Read: plain load — TSX or SGL provides isolation
uint8_t  tm_read_i1(volatile uint8_t *addr)  { return *addr; }
uint16_t tm_read_i2(volatile uint16_t *addr) { return *addr; }
uint32_t tm_read_i4(volatile uint32_t *addr) { return *addr; }
uint64_t tm_read_i8(volatile uint64_t *addr) { return *addr; }
float    tm_read_f4(volatile float *addr)    { return *addr; }
double   tm_read_f8(volatile double *addr)   { return *addr; }
void *   tm_read_ptr(volatile void **addr)   { return (void*)*addr; }

void *tm_read_z(volatile uint8_t *src, uint64_t len) {
    void *buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

// Write: plain store — TSX or SGL provides isolation
void tm_write_i1(volatile uint8_t *addr, uint8_t val)     { *addr = val; }
void tm_write_i2(volatile uint16_t *addr, uint16_t val)   { *addr = val; }
void tm_write_i4(volatile uint32_t *addr, uint32_t val)   { *addr = val; }
void tm_write_i8(volatile uint64_t *addr, uint64_t val)   { *addr = val; }
void tm_write_f4(volatile float *addr, float val)         { *addr = val; }
void tm_write_f8(volatile double *addr, double val)       { *addr = val; }
void tm_write_ptr(volatile void **addr, void *val)        { *addr = val; }

void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len) {
    memcpy((void*)dst, (const void*)src, len);
}

void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len) {
    memset((void*)addr, val, len);
}

void consume_ptr(volatile void *) {}

// TM allocator stubs — TSX and SGL both work with system malloc
void* tm_malloc(size_t size)   { return malloc(size); }
void* tm_calloc(size_t n, size_t s) { return calloc(n, s); }
void* tm_realloc(void *p, size_t s) { return realloc(p, s); }
void  tm_free(void *p)              { free(p); }

} // extern "C"
