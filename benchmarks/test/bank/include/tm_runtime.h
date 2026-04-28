#pragma once
#include <cstdint>
#include <cstring>
#include <thread>

constexpr size_t TM_BUFFER_SIZE = 4096;

__thread uint8_t tm_tx_buffer[TM_BUFFER_SIZE];

extern "C" {
    void tm_init() {}
    void tm_exit() {}
    void tm_init_thread() {}
    void tm_exit_thread() {}
    void tm_begin() {}
    void tm_end() {}

    uint8_t tm_read_i1(volatile uint8_t *addr) { return *addr; }
    uint16_t tm_read_i2(volatile uint16_t *addr) { return *addr; }
    uint32_t tm_read_i4(volatile uint32_t *addr) { return *addr; }
    uint64_t tm_read_i8(volatile uint64_t *addr) { return *addr; }
    float tm_read_f4(volatile float *addr) { return *addr; }
    double tm_read_f8(volatile double *addr) { return *addr; }
    void *tm_read_ptr(volatile void **addr) { return *addr; }

    void *tm_read_z(volatile uint8_t *src, uint64_t len) {
        if (len < TM_BUFFER_SIZE) {
            memcpy(tm_tx_buffer, (void*)src, len);
            return tm_tx_buffer;
        }
        void *buf = malloc(len);
        memcpy(buf, src, len);
        return buf;
    }

    void tm_write_i1(volatile uint8_t *addr, uint8_t val) { *addr = val; }
    void tm_write_i2(volatile uint16_t *addr, uint16_t val) { *addr = val; }
    void tm_write_i4(volatile uint32_t *addr, uint32_t val) { *addr = val; }
    void tm_write_i8(volatile uint64_t *addr, uint64_t val) { *addr = val; }
    void tm_write_f4(volatile float *addr, float val) { *addr = val; }
    void tm_write_f8(volatile double *addr, double val) { *addr = val; }
    void tm_write_ptr(volatile void **addr, void *val) { *addr = val; }

    void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len) {
        memcpy((void*)dst, (void*)src, len);
    }

    void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len) {
        memset((void*)addr, val, len);
    }

    void consume_ptr(volatile void *ptr) { (void)ptr; }
}