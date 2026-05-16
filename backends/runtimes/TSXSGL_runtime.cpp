/**
 * TSX+SGL Runtime for LLVM TM Plugin
 *
 * Hybrid runtime that uses Intel RTM (Restricted Transactional Memory)
 * for the fast path, falling back to a single global mutex after 5
 * aborts.  tm_read/tm_write are plain loads/stores — TSX provides
 * isolation at the cache-line level, and the SGL fallback gives
 * exclusive access.
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

// _xbegin, _xend, _xabort, _xtest
#if defined(__x86_64__) || defined(__i386__)
  #include <immintrin.h>
#endif

thread_local bool g_in_tx = false;

static std::mutex global_tx_lock;

// Plugin-required thread-local state
__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;

// Whether the current outer transaction is running in TSX (vs SGL fallback)
thread_local bool in_tsx = false;

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
    if (tm_nested_call_counter > 0) {
        tm_nested_call_counter++;
        return;
    }
    tm_nested_call_counter = 1;
    g_in_tx = true;

#if defined(__x86_64__) || defined(__i386__)
    for (int attempts = 0; attempts < 5; attempts++) {
        unsigned status = _xbegin();
        if (status == _XBEGIN_STARTED) {
            in_tsx = true;
            return;
        }
        // Only retry if the abort reason suggests it could help
        if (!(status & _XABORT_RETRY))
            break;
    }
#else
    (void)in_tsx; // suppress unused warning
#endif
    // Fall back to single global lock
    global_tx_lock.lock();
    in_tsx = false;
}

void tm_end() {
    assert(tm_nested_call_counter >= 0);
    if (tm_nested_call_counter > 1) {
        tm_nested_call_counter--;
        return;
    }
    tm_nested_call_counter = 0;
    g_in_tx = false;

    if (in_tsx) {
#if defined(__x86_64__) || defined(__i386__)
        _xend();
#endif
        return;
    }
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
