/**
 * SwissTM-style Runtime Wrapper for LLVM TM Plugin
 * 
 * This file provides C++ wrapper functions for a SwissTM-style STM implementation.
 * Uses the modern C++17 atomic primitives from tm_atomic.h
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <csetjmp>
#include <atomic>

#include "../../include/tm_atomic.h"

using namespace tm_atomic;

static constexpr uint64_t MAX_THREADS = 256;

struct SwisstmThread {
    sigjmp_buf jmp_buf;
    bool tx_active = false;
    bool read_only = false;
    uint64_t start_time = 0;
};

static std::atomic<uint64_t> global_epoch{0};
static std::atomic<uint64_t> commit_count{0};
static std::atomic<bool> initialized{false};
static thread_local SwisstmThread *current_tx = nullptr;

extern "C" void tm_init() {
    if (!initialized.load(std::memory_order_relaxed)) {
        initialized.store(true, std::memory_order_seq_cst);
    }
}

extern "C" void tm_exit() {
    initialized.store(false, std::memory_order_seq_cst);
}

extern "C" void tm_init_thread() {
    current_tx = new SwisstmThread();
}

extern "C" void tm_exit_thread() {
    if (current_tx) {
        delete current_tx;
        current_tx = nullptr;
    }
}

extern "C" void tm_begin() {
    if (!current_tx) {
        current_tx = new SwisstmThread();
    }
    
    if (current_tx->tx_active) {
        return;
    }
    
    current_tx->tx_active = true;
    current_tx->read_only = false;
    current_tx->start_time = global_epoch.load(std::memory_order_relaxed);
    
    if (sigsetjmp(current_tx->jmp_buf, 0) == 0) {
        
    } else {
        current_tx->tx_active = false;
    }
}

extern "C" void tm_end() {
    if (current_tx && current_tx->tx_active) {
        global_epoch.fetch_add(1, std::memory_order_seq_cst);
        commit_count.fetch_add(1, std::memory_order_relaxed);
        current_tx->tx_active = false;
    }
}

extern "C" uint8_t tm_read_i1(volatile uint8_t *addr) {
    return *addr;
}

extern "C" uint16_t tm_read_i2(volatile uint16_t *addr) {
    return *addr;
}

extern "C" uint32_t tm_read_i4(volatile uint32_t *addr) {
    return load_full((atomic_t *)addr);
}

extern "C" uint64_t tm_read_i8(volatile uint64_t *addr) {
    return load_full((atomic_t *)addr);
}

extern "C" float tm_read_f4(volatile float *addr) {
    uint32_t bits = load_full((atomic_t *)addr);
    float result;
    std::memcpy(&result, &bits, sizeof(float));
    return result;
}

extern "C" double tm_read_f8(volatile double *addr) {
    uint64_t bits = load_full((atomic_t *)addr);
    double result;
    std::memcpy(&result, &bits, sizeof(double));
    return result;
}

extern "C" void *tm_read_ptr(volatile void **addr) {
    return load_full((atomic_t *)addr);
}

extern "C" void *tm_read_z(volatile uint8_t *src, uint64_t len) {
    void *buffer = std::malloc(len);
    for (uint64_t i = 0; i < len; ++i) {
        ((uint8_t *)buffer)[i] = tm_read_i1(src + i);
    }
    return buffer;
}

extern "C" void tm_write_i1(volatile uint8_t *addr, uint8_t val) {
    store_full((atomic_t *)addr, val);
}

extern "C" void tm_write_i2(volatile uint16_t *addr, uint16_t val) {
    store_full((atomic_t *)addr, val);
}

extern "C" void tm_write_i4(volatile uint32_t *addr, uint32_t val) {
    store_full((atomic_t *)addr, val);
}

extern "C" void tm_write_i8(volatile uint64_t *addr, uint64_t val) {
    store_full((atomic_t *)addr, val);
}

extern "C" void tm_write_f4(volatile float *addr, float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, sizeof(float));
    store_full((atomic_t *)addr, bits);
}

extern "C" void tm_write_f8(volatile double *addr, double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(double));
    store_full((atomic_t *)addr, bits);
}

extern "C" void tm_write_ptr(volatile void **addr, void *val) {
    store_full((atomic_t *)addr, (uintptr_t)val);
}

extern "C" void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len) {
    for (uint64_t i = 0; i < len; ++i) {
        tm_write_i1(dst + i, src[i]);
    }
}

extern "C" void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len) {
    for (uint64_t i = 0; i < len; ++i) {
        store_full((atomic_t *)(addr + i), val);
    }
}