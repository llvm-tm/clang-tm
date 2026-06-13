/**
 * SingleGlobalLock Runtime for LLVM TM Plugin
 *
 * This runtime uses a single global lock to protect transactions.
 * The lock is only acquired at the outermost transaction level to avoid deadlocks.
 * No read/write instrumentation is needed - the global lock provides exclusive access.
 */

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <csetjmp>

#include "tm_thread_state.hpp"
#include "tm_hooks.hpp"
#include "tm_region_allocator.hpp"
#include "tm_alloc_overrides.hpp"
thread_local bool g_in_tx = false;

static std::mutex global_tx_lock;
static std::atomic<bool> initialized{false};

static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_tx_count{0};

// Plugin required
struct TMThreadState;
extern "C" TMThreadState *tm_get_thread_state();
__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;
static thread_local TMThreadState g_tm_state{0, 0};

extern const TMRealHooks g_sgl_hooks;

extern "C" {

TMThreadState *tm_get_thread_state() {
    return &g_tm_state;
}

void tm_init() {
    if (!initialized.load(std::memory_order_relaxed)) {
        initialized.store(true, std::memory_order_seq_cst);
    }
    tm_register_real_hooks(&g_sgl_hooks);
}

void tm_init_thread() {
    tm_hook_init_thread();
}

void tm_exit() {
    initialized.store(false, std::memory_order_seq_cst);
}

void tm_exit_thread() {
    tm_hook_exit_thread();
}

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }

void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp() {
    return 0;
}

void tm_set_jmpbuf(void *buf) { }

sigjmp_buf* tm_get_env() {
    return (sigjmp_buf*)&tm_jmpbuf;
}

void tm_set_env(sigjmp_buf* env) {
    if (env) {
        memcpy(&tm_jmpbuf, env, sizeof(tm_jmpbuf));
    }
}

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {
}

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

void consume_ptr(volatile void *ptr) { (void)ptr; }

} // extern "C"

// Hook implementations (static; registered via tm_register_real_hooks)
static void real_tm_begin() {
    g_in_tx = true;
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
    global_tx_lock.lock();
    assert(g_tm_state.nested_call_counter >= 0);
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

static void real_tm_end() {
    global_tx_lock.unlock();
    g_in_tx = false;
    assert(g_tm_state.nested_call_counter >= 0);
}

static uint8_t real_tm_read_i1(uint8_t *addr) { return *addr; }
static uint16_t real_tm_read_i2(uint16_t *addr) { return *addr; }
static uint32_t real_tm_read_i4(uint32_t *addr) { return *addr; }
static uint64_t real_tm_read_i8(uint64_t *addr) { return *addr; }
static float real_tm_read_f4(float *addr) { return *addr; }
static double real_tm_read_f8(double *addr) { return *addr; }
static void *real_tm_read_ptr(void **addr) { return *addr; }

static void real_tm_write_i1(uint8_t *addr, uint8_t val) { *addr = val; }
static void real_tm_write_i2(uint16_t *addr, uint16_t val) { *addr = val; }
static void real_tm_write_i4(uint32_t *addr, uint32_t val) { *addr = val; }
static void real_tm_write_i8(uint64_t *addr, int64_t val) { *addr = val; }
static void real_tm_write_f4(float *addr, float val) { *addr = val; }
static void real_tm_write_f8(double *addr, double val) { *addr = val; }
static void real_tm_write_ptr(void **addr, void *val) { *addr = val; }

static void *real_tm_malloc(size_t size) {
    void *p = stm::tm_region_malloc(size);
    if (p) { std::memset(p, 0, size); tm_track_spec_alloc(p); }
    return p;
}
static void *real_tm_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = stm::tm_region_malloc(total);
    if (p) { std::memset(p, 0, total); tm_track_spec_alloc(p); }
    return p;
}
static void *real_tm_realloc(void *ptr, size_t size) {
    if (!ptr) return real_tm_malloc(size);
    void *p = stm::tm_region_malloc(size);
    if (p) { std::memcpy(p, ptr, size); stm::tm_region_free(ptr); tm_track_spec_alloc(p); }
    return p;
}
static void  real_tm_free(void *ptr) {
    if (!ptr) return;
    tm_untrack_spec_alloc(ptr);
    if (g_in_tx)
        tm_free_append_deferred(ptr);
    else
        stm::tm_region_free(ptr);
}

const TMRealHooks g_sgl_hooks = {
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
