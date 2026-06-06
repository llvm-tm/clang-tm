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

#include "tm_alloc_overrides.hpp"
#include "tm_thread_state.hpp"
thread_local bool g_in_tx = false;

static std::mutex global_tx_lock;
static std::atomic<bool> initialized{false};

static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_tx_count{0};

// Plugin required
__thread std::jmp_buf tm_jmpbuf;
static thread_local TMThreadState g_tm_state{0, 0};

extern "C" {

TMThreadState *tm_get_thread_state() {
    return &g_tm_state;
}

void tm_init() {
    if (!initialized.load(std::memory_order_relaxed)) {
        initialized.store(true, std::memory_order_seq_cst);
    }
}

void tm_init_thread() {
}

void tm_exit() {
    initialized.store(false, std::memory_order_seq_cst);
}

void tm_exit_thread() {
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

void tm_begin() {
    g_in_tx = true;
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
    global_tx_lock.lock();
    assert(g_tm_state.nested_call_counter >= 0);
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
}

void tm_end()
{
    global_tx_lock.unlock();
    g_in_tx = false;
    assert(g_tm_state.nested_call_counter >= 0);
}

// Read functions with symbol_id parameter (unused in this backend)
uint8_t tm_read_i1(volatile uint8_t *addr) { return *addr; }
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
float tm_read_f4(volatile float *addr) { return *addr; }
double tm_read_f8(volatile double *addr) { return *addr; }
void *tm_read_ptr(volatile void **addr) { return (void*)*addr; }

void *tm_read_z(volatile uint8_t *src, uint64_t len) {
    void *buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

// Write functions with symbol_id parameter (unused in this backend)
void tm_write_i1(volatile uint8_t *addr, uint8_t val) { *addr = val; }
void tm_write_i2(volatile uint16_t *addr, uint16_t val) { *addr = val; }
void tm_write_i4(volatile uint32_t *addr, uint32_t val) { *addr = val; }
void tm_write_i8(volatile uint64_t *addr, uint64_t val) { *addr = val; }
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
void tm_write_f4(volatile float *addr, float val) { *addr = val; }
void tm_write_f8(volatile double *addr, double val) { *addr = val; }
void tm_write_ptr(volatile void **addr, void *val) { *addr = val; }

void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len) {
    memcpy((void*)dst, (const void*)src, len);
}

void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len) {
    memset((void*)addr, val, len);
}

void consume_ptr(volatile void *ptr) { (void)ptr; }

// TM allocator stubs (redirect to system allocator)
void* tm_malloc(size_t size) { return g_in_tx ? malloc(size) : malloc(size); }
void* tm_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void* tm_realloc(void* ptr, size_t size) { return realloc(ptr, size); }
void  tm_free(void* ptr) { free(ptr); }

} // extern "C"