/**
 * TL2 STM Runtime Wrapper for LLVM TM Plugin
 * 
 * This file provides C++ wrapper functions for TL2 STM operations.
 * It bridges the LLVM instrumentation plugin's transaction interface
 * with the TL2 STM implementation.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <csetjmp>

// Forward declarations for TL2
extern "C" {
    typedef struct thread_s Thread;
    
    // TL2 transaction management
    Thread *TxNewThread();
    void TxFreeThread(Thread *tx);
    void TxInitThread(Thread *tx, long id);
    void TxOnce();
    void TxShutdown();
    
    // TL2 read/write operations
    void TxStart(Thread *tx, sigjmp_buf *env, int *readonly);
    void TxAbort(Thread *tx);
    int TxCommit(Thread *tx);
    intptr_t TxLoad(Thread *tx, volatile intptr_t *addr);
    void TxStore(Thread *tx, volatile intptr_t *addr, intptr_t val);
}

// Thread-local storage for STM thread context
thread_local Thread *__tl2_thread = nullptr;
thread_local bool __tl2_initialized = false;
thread_local sigjmp_buf __tl2_jmp_env;
thread_local int __tl2_readonly = 0;
thread_local bool __tl2_tx_active = false;

// Nested transaction counter (required by plugin)
// Both single and double underscore versions for compatibility
thread_local int32_t tm_nested_call_counter = 0;
thread_local int32_t __tm_nested_call_counter = 0;

// Global initialization flag
static bool __tl2_global_init = false;

/**
 * Initialize the TL2 STM system
 */
extern "C" void tm_init() {
    if (!__tl2_global_init) {
        TxOnce();
        __tl2_global_init = true;
    }
}

/**
 * Shut down the TL2 STM system
 */
extern "C" void tm_exit() {
    if (__tl2_global_init) {
        TxShutdown();
        __tl2_global_init = false;
    }
}

/**
 * Initialize thread-local STM state
 */
extern "C" void tm_init_thread() {
    if (!__tl2_initialized) {
        __tl2_thread = TxNewThread();
        if (__tl2_thread) {
            long tid = std::hash<std::thread::id>()(std::this_thread::get_id()) % 1000000;
            TxInitThread(__tl2_thread, tid);
            __tl2_initialized = true;
        }
    }
}

/**
 * Cleanup thread-local STM state
 */
extern "C" void tm_exit_thread() {
    if (__tl2_initialized && __tl2_thread) {
        TxFreeThread(__tl2_thread);
        __tl2_thread = nullptr;
        __tl2_initialized = false;
    }
}

/**
 * Begin a transaction
 */
extern "C" void tm_begin() {
    if (!__tl2_initialized) {
        tm_init_thread();
    }
    if (!__tl2_tx_active) {
        TxStart(__tl2_thread, &__tl2_jmp_env, &__tl2_readonly);
        if (sigsetjmp(__tl2_jmp_env, 0) == 0) {
            __tl2_tx_active = true;
        } else {
            __tl2_tx_active = false;
        }
    }
}

/**
 * End a transaction (commit)
 */
extern "C" void tm_end() {
    if (__tl2_tx_active) {
        TxCommit(__tl2_thread);
        __tl2_tx_active = false;
    }
}

/**
 * Read an 8-bit value from a transactional address
 */
extern "C" uint8_t tm_read_i1(volatile uint8_t *addr) {
    return (uint8_t)TxLoad(__tl2_thread, (volatile intptr_t *)addr);
}

/**
 * Read a 16-bit value from a transactional address
 */
extern "C" uint16_t tm_read_i2(volatile uint16_t *addr) {
    return (uint16_t)TxLoad(__tl2_thread, (volatile intptr_t *)addr);
}

/**
 * Read a 32-bit value from a transactional address
 */
extern "C" uint32_t tm_read_i4(volatile uint32_t *addr) {
    return (uint32_t)TxLoad(__tl2_thread, (volatile intptr_t *)addr);
}

/**
 * Read a 64-bit value from a transactional address
 */
extern "C" uint64_t tm_read_i8(volatile uint64_t *addr) {
    return (uint64_t)TxLoad(__tl2_thread, (volatile intptr_t *)addr);
}

/**
 * Read a 32-bit float from a transactional address
 */
extern "C" float tm_read_f4(volatile float *addr) {
    uint32_t bits = (uint32_t)TxLoad(__tl2_thread, (volatile intptr_t *)addr);
    float result;
    std::memcpy(&result, &bits, sizeof(float));
    return result;
}

/**
 * Read a 64-bit double from a transactional address
 */
extern "C" double tm_read_f8(volatile double *addr) {
    uint64_t bits = (uint64_t)TxLoad(__tl2_thread, (volatile intptr_t *)addr);
    double result;
    std::memcpy(&result, &bits, sizeof(double));
    return result;
}

/**
 * Read a pointer from a transactional address
 */
extern "C" void *tm_read_ptr(volatile void **addr) {
    return (void *)TxLoad(__tl2_thread, (volatile intptr_t *)addr);
}

/**
 * Read a block of memory from a transactional address
 */
extern "C" void *tm_read_z(volatile uint8_t *src, uint64_t len) {
    void *buffer = std::malloc(len);
    for (uint64_t i = 0; i < len; ++i) {
        ((uint8_t *)buffer)[i] = tm_read_i1(src + i);
    }
    return buffer;
}

/**
 * Write an 8-bit value to a transactional address
 */
extern "C" void tm_write_i1(volatile uint8_t *addr, uint8_t val) {
    TxStore(__tl2_thread, (volatile intptr_t *)addr, (intptr_t)val);
}

/**
 * Write a 16-bit value to a transactional address
 */
extern "C" void tm_write_i2(volatile uint16_t *addr, uint16_t val) {
    TxStore(__tl2_thread, (volatile intptr_t *)addr, (intptr_t)val);
}

/**
 * Write a 32-bit value to a transactional address
 */
extern "C" void tm_write_i4(volatile uint32_t *addr, uint32_t val) {
    TxStore(__tl2_thread, (volatile intptr_t *)addr, (intptr_t)val);
}

/**
 * Write a 64-bit value to a transactional address
 */
extern "C" void tm_write_i8(volatile uint64_t *addr, uint64_t val) {
    TxStore(__tl2_thread, (volatile intptr_t *)addr, (intptr_t)val);
}

/**
 * Write a 32-bit float to a transactional address
 */
extern "C" void tm_write_f4(volatile float *addr, float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, sizeof(float));
    TxStore(__tl2_thread, (volatile intptr_t *)addr, (intptr_t)bits);
}

/**
 * Write a 64-bit double to a transactional address
 */
extern "C" void tm_write_f8(volatile double *addr, double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(double));
    TxStore(__tl2_thread, (volatile intptr_t *)addr, (intptr_t)bits);
}

/**
 * Write a pointer to a transactional address
 */
extern "C" void tm_write_ptr(volatile void **addr, void *val) {
    TxStore(__tl2_thread, (volatile intptr_t *)addr, (intptr_t)val);
}

/**
 * Write a block of memory to a transactional address
 */
extern "C" void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len) {
    for (uint64_t i = 0; i < len; ++i) {
        tm_write_i1(dst + i, src[i]);
    }
}

/**
 * Transactional memset operation
 */
extern "C" void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len) {
    for (uint64_t i = 0; i < len; ++i) {
        tm_write_i1(addr + i, val);
    }
}
