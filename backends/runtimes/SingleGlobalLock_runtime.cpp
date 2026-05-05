/**
 * SingleGlobalLock Runtime for LLVM TM Plugin
 *
 * This runtime uses a single global lock to protect transactions.
 * The lock is only acquired at the outermost transaction level to avoid deadlocks.
 * No read/write instrumentation is needed - the global lock provides exclusive access.
 */

#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <csetjmp>

static std::mutex global_tx_lock;
static std::atomic<bool> initialized{false};

// Plugin required
__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;

extern "C" {

void tm_init() {
    fprintf(stderr, "SingleGlobalLock: tm_init called\n");
    fflush(stderr);
    if (!initialized.load(std::memory_order_relaxed)) {
        initialized.store(true, std::memory_order_seq_cst);
    }
    fprintf(stderr, "SingleGlobalLock: tm_init done\n");
    fflush(stderr);
}

void tm_init_thread() {
    fprintf(stderr, "SingleGlobalLock: tm_init_thread called\n");
    fflush(stderr);
}

void tm_exit() {
    initialized.store(false, std::memory_order_seq_cst);
}

void tm_exit_thread() {
}

int tm_setjmp() {
    return 0;
}

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
    fprintf(stderr, "SingleGlobalLock: tm_begin tm_nested_call_counter=%d\n", tm_nested_call_counter);
    if (tm_nested_call_counter == 1) {
        global_tx_lock.lock();
    }
}

void tm_end() {
    fprintf(stderr, "SingleGlobalLock: tm_end tm_nested_call_counter=%d\n", tm_nested_call_counter);
    if (tm_nested_call_counter == 1) {
        global_tx_lock.unlock();
    }
}

// Read functions without symbol_id parameter
uint8_t tm_read_i1(volatile uint8_t *addr) { return *addr; }
uint16_t tm_read_i2(volatile uint16_t *addr) { return *addr; }
uint32_t tm_read_i4(volatile uint32_t *addr) { return *addr; }
uint64_t tm_read_i8(volatile uint64_t *addr) { return *addr; }
float tm_read_f4(volatile float *addr) { return *addr; }
double tm_read_f8(volatile double *addr) { return *addr; }
void *tm_read_ptr(volatile void **addr) { return (void*)*addr; }

void *tm_read_z(volatile uint8_t *src, uint64_t len) {
    void *buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

// Write functions without symbol_id parameter
void tm_write_i1(volatile uint8_t *addr, uint8_t val) { *addr = val; }
void tm_write_i2(volatile uint16_t *addr, uint16_t val) { *addr = val; }
void tm_write_i4(volatile uint32_t *addr, uint32_t val) { *addr = val; }
void tm_write_i8(volatile uint64_t *addr, uint64_t val) { *addr = val; }
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

} // extern "C"