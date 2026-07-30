/**
 * POWER8 HTM + SGL Runtime for TM API
 *
 * Uses POWER8 Hardware Transactional Memory with single-global-lock
 * fallback.  Pattern:
 *   1. Try __builtin_tbegin(0) up to 5 times.
 *   2. If all fail, fall back to global_tx_lock (SGL).
 *   3. On tm_end(), commit via __builtin_tend(0) or release the lock.
 *
 * Build with:  powerpc64le-linux-gnu-g++ -mcpu=power8 -I... -pthread ...
 *
 * Hardware requirement: POWER8 or newer with HTM.
 */

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <csetjmp>
#include <unordered_set>

#include "../common/tm_thread_state.hpp"
#include "../common/tm_hooks.hpp"
#include "../common/tm_region_allocator.hpp"
#include "../common/tm_alloc_overrides.hpp"

extern const TMRealHooks g_power8htm_hooks;

thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;
thread_local bool in_htm = false;

static std::mutex global_tx_lock;
static std::atomic<uint64_t> sgl_owner{0};

// Plugin-required thread-local state (defined in tm_hooks.cpp)
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

static void *real_tm_get_thread_state() {
    return (void*)&tm_nested_call_counter;
}

extern "C" {

#ifdef LLVM_TM_PLUGIN
static void do_tm_init();
static void do_tm_exit();
static void do_tm_init_thread();
static void do_tm_exit_thread();

void (*tm_init)()        = do_tm_init;
void (*tm_exit)()        = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;

static void do_tm_init()
#else
void tm_init()
#endif
{
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed\n");
        abort();
    }
    tm_register_real_hooks(&g_power8htm_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{ tm_hook_init_thread(); tm_nested_call_counter = 0; in_htm = false; }

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{ tm_hook_exit_thread(); }

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }
void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp() { return 0; }

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

#if defined(__powerpc64__) || defined(__ppc64__) || defined(_ARCH_PPC64)
    // Retry counter in thread-local memory so it survives POWER8's
    // register checkpoint restoration on abort.
    for (int retries = 0; retries < 5; retries++) {
        if (__builtin_tbegin(0)) {
            in_htm = true;
            return;
        }
        // On abort from a successful tbegin., register state is restored
        // but memory writes (like retries++) survive.  Spin briefly for
        // contention before retrying.
        for (volatile int spin = 0; spin < 100; spin++) {
            __asm__ __volatile__("nop" ::: "memory");
        }
    }
#else
    (void)in_htm;
#endif

    global_tx_lock.lock();
    sgl_owner.store(1, std::memory_order_seq_cst);
    in_htm = false;
}

static void real_tm_end() {
    if (tm_nested_call_counter > 1) return;
    g_in_tx = false;

    if (in_htm) {
#if defined(__powerpc64__) || defined(__ppc64__) || defined(_ARCH_PPC64)
        __builtin_tend(0);
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

const TMRealHooks g_power8htm_hooks = {
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
    .get_thread_state = real_tm_get_thread_state,
};
