/**
 * TSX+SGL Runtime for LLVM TM Plugin
 *
 * Standard TSX lock-elision pattern with a thread-ID lock variable:
 *   1. _xbegin() and, if successful, read sgl_owner.  If sgl_owner != 0
 *      (lock held), _xabort() immediately.
 *   2. The sgl_owner cache-line in the TSX read set ensures that any
 *      concurrent SGL entry (which writes to sgl_owner) aborts the TSX.
 *   3. After an explicit lock-busy abort, spin-wait for the lock to
 *      become free before retrying (anti-lemming effect).
 *   4. At tm_end() the sgl_owner is re-read; if it changed, the TSX
 *      was aborted by the hardware already (first write to sgl_owner
 *      triggers cache-coherence abort), but we _xabort() as a safety net.
 *   5. When all retries are exhausted we acquire global_tx_lock (SGL)
 *      and write 1 to sgl_owner so concurrent TSX transactions abort.
 *      On SGL release we write 0 back.
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

#include "../common/tm_thread_state.hpp"

thread_local bool g_in_tx = false;
thread_local bool in_tsx = false;
thread_local uint64_t tsx_start_owner = 0;

static std::mutex global_tx_lock;
static std::atomic<uint64_t> sgl_owner{0};

// Plugin-required thread-local state
__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;

extern "C" TMThreadState *tm_get_thread_state() {
    return reinterpret_cast<TMThreadState*>(&tm_nested_call_counter);
}

enum { LOCK_BUSY = 0xFF, OWNER_CHANGED = 0x01 };

extern "C" {

void tm_init() {}
void tm_exit() {}
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
            // Read sgl_owner into the TSX read-set AND check if
            // the lock is held.
            uint64_t v = sgl_owner.load(std::memory_order_seq_cst);
            if (v != 0) {
                _xabort(LOCK_BUSY);
            }
            tsx_start_owner = v;
            in_tsx = true;
            return;
        }


        if ((status & _XABORT_EXPLICIT) &&
            _XABORT_CODE(status) == LOCK_BUSY) {
            // Lock was busy — wait for it to become free before retrying
            // to avoid the lemming effect (thundering herd into SGL).
            while (sgl_owner.load(std::memory_order_relaxed) != 0)
                _mm_pause();
        } else if (!(status & _XABORT_RETRY)) {
            break;
        }
    }
#else
    (void)in_tsx;
#endif
    // Fallback: enter SGL (write to sgl_owner so TSX threads abort).
    global_tx_lock.lock();
    sgl_owner.store(1, std::memory_order_seq_cst);
    in_tsx = false;
}

void tm_end() {
    if (tm_nested_call_counter > 1) return;
    g_in_tx = false;

    if (in_tsx) {
#if defined(__x86_64__) || defined(__i386__)
        if (sgl_owner.load(std::memory_order_seq_cst) != tsx_start_owner) {
            _xabort(OWNER_CHANGED);
        }
        _xend();
#endif
        return;
    }
    sgl_owner.store(0, std::memory_order_seq_cst);
    global_tx_lock.unlock();
}

// Read: plain load — TSX or SGL provides isolation
uint8_t  tm_read_i1(volatile uint8_t *addr)  { return *addr; }
uint16_t tm_read_i2(volatile uint16_t *addr) { return *addr; }
uint32_t tm_read_i4(volatile uint32_t *addr) { return *addr; }
uint64_t tm_read_i8(volatile uint64_t *addr) { return *addr; }
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
