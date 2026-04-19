/**
 * TinySTM Runtime Wrapper for LLVM TM Plugin
 * 
 * This file provides C++ wrapper functions for TinySTM operations.
 * It bridges the LLVM instrumentation plugin's transaction interface
 * with the TinySTM implementation.
 * 
 * Uses modern C++17 memory ordering from tm_atomic.h for proper synchronization.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "../include/tm_atomic.h"

// TinySTM C API
extern "C" {
    #include "stm.h"
    
    // TinySTM provides:
    // stm_init() - Initialize STM system
    // stm_exit() - Shutdown STM system
    // stm_init_thread() - Initialize thread
    // stm_exit_thread() - Cleanup thread
    // stm_start(tx_attr) - Start transaction
    // stm_commit() - Commit transaction
    // stm_abort(tx_id) - Abort transaction
    // stm_load(addr) - Transactional load
    // stm_store(addr, val) - Transactional store
}

// Thread-local transaction context with proper memory ordering
thread_local int __tinystm_tx_active = 0;
thread_local sigjmp_buf *__tinystm_jmp_env = nullptr;
thread_local sigjmp_buf __tinystm_local_jmp;

// Global initialization flag with acquire-release semantics
static std::atomic<bool> __tinystm_global_init{false};

/**
 * Initialize the TinySTM system
 * Uses seq_cst for proper visibility across threads
 */
extern "C" void tm_init() {
    bool expected = false;
    if (__tinystm_global_init.compare_exchange_strong(expected, true, 
            std::memory_order_seq_cst, std::memory_order_relaxed)) {
        stm_init();
    }
}

/**
 * Shut down the TinySTM system
 */
extern "C" void tm_exit() {
    if (__tinystm_global_init.load(std::memory_order_seq_cst)) {
        stm_exit();
        __tinystm_global_init.store(false, std::memory_order_seq_cst);
    }
}

/**
 * Initialize thread-local STM state
 * Full memory barrier to ensure visibility
 */
extern "C" void tm_init_thread() {
    stm_init_thread();
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

/**
 * Cleanup thread-local STM state
 */
extern "C" void tm_exit_thread() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    stm_exit_thread();
}

/**
 * Begin a transaction
 *
 * Flow when NOT using abort/retry:
 * 1. Plugin injects setjmp(tm_jmpbuf) at function entry
 * 2. setjmp returns 0 (first attempt)
 * 3. We call stm_start() to initialize TinySTM tx
 * 4. Transaction body runs
 * 5. On abort (shouldn't happen normally): we just retry from plugin's setjmp
 */
extern "C" void tm_begin() {
    if (__tinystm_tx_active) {
        return;
    }

    stm_tx_attr_t attr = {0};
    stm_start(attr);
    __tinystm_tx_active = 1;
}

/**
 * End a transaction (commit)
 */
extern "C" void tm_end() {
    if (__tinystm_tx_active) {
        stm_commit();
        __tinystm_tx_active = 0;
    }
}

/**
 * Read an 8-bit value from a transactional address
 */
extern "C" uint8_t tm_read_i1(volatile uint8_t *addr) {
    return (uint8_t)stm_load((stm_word_t *)addr);
}

/**
 * Read a 16-bit value from a transactional address
 */
extern "C" uint16_t tm_read_i2(volatile uint16_t *addr) {
    return (uint16_t)stm_load((stm_word_t *)addr);
}

/**
 * Read a 32-bit value from a transactional address
 */
extern "C" uint32_t tm_read_i4(volatile uint32_t *addr) {
    return (uint32_t)stm_load((stm_word_t *)addr);
}

/**
 * Read a 64-bit value from a transactional address
 */
extern "C" uint64_t tm_read_i8(volatile uint64_t *addr) {
    return (uint64_t)stm_load((stm_word_t *)addr);
}

/**
 * Read a 32-bit float from a transactional address
 */
extern "C" float tm_read_f4(volatile float *addr) {
    uint32_t bits = (uint32_t)stm_load((stm_word_t *)addr);
    float result;
    std::memcpy(&result, &bits, sizeof(float));
    return result;
}

/**
 * Read a 64-bit double from a transactional address
 */
extern "C" double tm_read_f8(volatile double *addr) {
    uint64_t bits = (uint64_t)stm_load((stm_word_t *)addr);
    double result;
    std::memcpy(&result, &bits, sizeof(double));
    return result;
}

/**
 * Read a pointer from a transactional address
 */
extern "C" void *tm_read_ptr(volatile void **addr) {
    return (void *)stm_load((stm_word_t *)addr);
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
    stm_store((stm_word_t *)addr, (stm_word_t)val);
}

/**
 * Write a 16-bit value to a transactional address
 */
extern "C" void tm_write_i2(volatile uint16_t *addr, uint16_t val) {
    stm_store((stm_word_t *)addr, (stm_word_t)val);
}

/**
 * Write a 32-bit value to a transactional address
 */
extern "C" void tm_write_i4(volatile uint32_t *addr, uint32_t val) {
    stm_store((stm_word_t *)addr, (stm_word_t)val);
}

/**
 * Write a 64-bit value to a transactional address
 */
extern "C" void tm_write_i8(volatile uint64_t *addr, uint64_t val) {
    stm_store((stm_word_t *)addr, (stm_word_t)val);
}

/**
 * Write a 32-bit float to a transactional address
 */
extern "C" void tm_write_f4(volatile float *addr, float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, sizeof(float));
    stm_store((stm_word_t *)addr, (stm_word_t)bits);
}

/**
 * Write a 64-bit double to a transactional address
 */
extern "C" void tm_write_f8(volatile double *addr, double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(double));
    stm_store((stm_word_t *)addr, (stm_word_t)bits);
}

/**
 * End a transaction (commit)
 */
extern "C" void tm_end() {
    if (__tinystm_tx_active) {
        stm_commit();
        __tinystm_tx_active = 0;
    }
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
