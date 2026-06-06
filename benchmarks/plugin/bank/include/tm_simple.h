#pragma once
#include <cstdint>
#include <cstring>

#define TM_BUFFER_SIZE 4096

__thread uint8_t tm_buffer[TM_BUFFER_SIZE];

extern "C" void tm_init() {}
extern "C" void tm_exit() {}
extern "C" void tm_init_thread() {}
extern "C" void tm_exit_thread() {}
extern "C" void tm_begin() {}
extern "C" void tm_end() {}

extern "C" uint8_t tm_read_i1(volatile uint8_t *addr) { return *addr; }
extern "C" uint16_t tm_read_i2(volatile uint16_t *addr) { return *addr; }
extern "C" uint32_t tm_read_i4(volatile uint32_t *addr) { return *addr; }
extern "C" uint64_t tm_read_i8(volatile uint64_t *addr) { return *addr; }
extern "C" float tm_read_f4(volatile float *addr) { return *addr; }
extern "C" double tm_read_f8(volatile double *addr) { return *addr; }
extern "C" void *tm_read_ptr(volatile void **addr) { return *addr; }

extern "C" void *tm_read_z(volatile uint8_t *src, uint64_t len) {
    if (len < TM_BUFFER_SIZE) {
        memcpy(tm_buffer, (void*)src, len);
        return tm_buffer;
    }
    void *buf = malloc(len);
    memcpy(buf, src, len);
    return buf;
}

extern "C" void tm_write_i1(volatile uint8_t *addr, uint8_t val) { *addr = val; }
extern "C" void tm_write_i2(volatile uint16_t *addr, uint16_t val) { *addr = val; }
extern "C" void tm_write_i4(volatile uint32_t *addr, uint32_t val) { *addr = val; }
extern "C" void tm_write_i8(volatile uint64_t *addr, uint64_t val) { *addr = val; }
extern "C" void tm_write_f4(volatile float *addr, float val) { *addr = val; }
extern "C" void tm_write_f8(volatile double *addr, double val) { *addr = val; }
extern "C" void tm_write_ptr(volatile void **addr, void *val) { *addr = val; }

extern "C" void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len) {
    memcpy((void*)dst, (void*)src, len);
}

extern "C" void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len) {
    memset((void*)addr, val, len);
}