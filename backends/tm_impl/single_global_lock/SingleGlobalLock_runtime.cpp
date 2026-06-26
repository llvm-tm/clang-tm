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
#include <unordered_set>

#include "tm_thread_state.hpp"
#include "tm_hooks.hpp"
#include "tm_region_allocator.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_backend_macros.hpp"
static std::mutex global_tx_lock;
static std::atomic<bool> initialized{false};

static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_tx_count{0};

// Plugin required
struct TMThreadState;
extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}
static thread_local TMThreadState g_tm_state{0, 0};

extern const TMRealHooks g_sgl_hooks;

extern "C" {

static void *real_tm_get_thread_state() {
    return (void*)&g_tm_state;
}

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
    if (!initialized.load(std::memory_order_relaxed)) {
        initialized.store(true, std::memory_order_seq_cst);
    }
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed\n");
        abort();
    }
    tm_register_real_hooks(&g_sgl_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
    tm_hook_init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    initialized.store(false, std::memory_order_seq_cst);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{
    tm_hook_exit_thread();
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

TM_REAL_HOOKS_TABLE(sgl)
