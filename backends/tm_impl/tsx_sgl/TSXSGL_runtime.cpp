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
#include "../common/tm_rtm.hpp"
#include "../common/tm_hooks.hpp"
#include "../common/tm_region_allocator.hpp"
#include "../common/tm_alloc_overrides.hpp"

extern const TMRealHooks g_tsxsgl_hooks;

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

void tm_init() { tm_register_real_hooks(&g_tsxsgl_hooks); }
void tm_exit() {}
void tm_init_thread() { tm_hook_init_thread(); tm_nested_call_counter = 0; in_tsx = false; }
void tm_exit_thread() { tm_hook_exit_thread(); }

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

void *tm_read_z(volatile uint8_t *src, uint64_t len) {
    void *buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

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

void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len) {
    memcpy((void*)dst, (const void*)src, len);
}

void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len) {
    memset((void*)addr, val, len);
}

void consume_ptr(volatile void *) {}

} // extern "C"

// ── Real implementations (registered via hooks) ─────────────────────

static void real_tm_begin() {
    if (tm_nested_call_counter > 1) return;
    g_in_tx = true;

#if defined(__x86_64__) || defined(__i386__)
    if (tm_rtm::available()) {
        for (int attempts = 0; attempts < 5; attempts++) {
            unsigned status = _xbegin();
            if (status == _XBEGIN_STARTED) {
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
                while (sgl_owner.load(std::memory_order_relaxed) != 0)
                    _mm_pause();
            } else if (!(status & _XABORT_RETRY)) {
                break;
            }
        }
    }
#else
    (void)in_tsx;
#endif
    global_tx_lock.lock();
    sgl_owner.store(1, std::memory_order_seq_cst);
    in_tsx = false;
}

static void real_tm_end() {
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

static uint8_t  real_tm_read_i1(uint8_t *addr)  { return *addr; }
static uint16_t real_tm_read_i2(uint16_t *addr) { return *addr; }
static uint32_t real_tm_read_i4(uint32_t *addr) { return *addr; }
static uint64_t real_tm_read_i8(uint64_t *addr) { return *addr; }
static float    real_tm_read_f4(float *addr)    { return *addr; }
static double   real_tm_read_f8(double *addr)   { return *addr; }
static void *   real_tm_read_ptr(void **addr)   { return (void*)*addr; }

static void real_tm_write_i1(uint8_t *addr, uint8_t val)     { *addr = val; }
static void real_tm_write_i2(uint16_t *addr, uint16_t val)   { *addr = val; }
static void real_tm_write_i4(uint32_t *addr, uint32_t val)   { *addr = val; }
static void real_tm_write_i8(uint64_t *addr, int64_t val)    { *(volatile uint64_t*)addr = (uint64_t)val; }
static void real_tm_write_f4(float *addr, float val)         { *addr = val; }
static void real_tm_write_f8(double *addr, double val)       { *addr = val; }
static void real_tm_write_ptr(void **addr, void *val)        { *addr = val; }

static void* real_tm_malloc(size_t size) {
    void* p = stm::tm_region_malloc(size);
    if (p) { std::memset(p, 0, size); tm_track_spec_alloc(p); }
    return p;
}
static void* real_tm_calloc(size_t n, size_t s) {
    size_t total = n * s;
    void* p = stm::tm_region_malloc(total);
    if (p) { std::memset(p, 0, total); tm_track_spec_alloc(p); }
    return p;
}
static void* real_tm_realloc(void *p, size_t s) {
    if (!p) return real_tm_malloc(s);
    void* np = stm::tm_region_malloc(s);
    if (np) { std::memcpy(np, p, s); stm::tm_region_free(p); tm_track_spec_alloc(np); }
    return np;
}
static void  real_tm_free(void *p) {
    if (!p) return;
    tm_untrack_spec_alloc(p);
    if (g_in_tx)
        tm_free_append_deferred(p);
    else
        stm::tm_region_free(p);
}

const TMRealHooks g_tsxsgl_hooks = {
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
    .write_i1  = real_tm_write_i1,
    .write_i2  = real_tm_write_i2,
    .write_i4  = real_tm_write_i4,
    .write_i8  = real_tm_write_i8,
    .write_f4  = real_tm_write_f4,
    .write_f8  = real_tm_write_f8,
    .write_ptr = real_tm_write_ptr,
};
