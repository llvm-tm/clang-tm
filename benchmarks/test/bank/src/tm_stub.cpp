#include <cstdint>
#include <cstring>
#include <thread>
#include <cstdlib>
#include <csetjmp>

#define TM_BUFFER_SIZE 4096

thread_local uint8_t tm_tx_buffer[TM_BUFFER_SIZE];
__thread sigjmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret = 0;

extern "C" void tm_init() {}
extern "C" void tm_exit() {}
extern "C" void tm_init_thread() {}
extern "C" void tm_exit_thread() {}
extern "C" void tm_begin() {}
extern "C" void tm_end() {}

extern "C" uint8_t tm_read_i1(volatile uint8_t *addr, uint32_t symbol_id) { return *addr; }
extern "C" uint16_t tm_read_i2(volatile uint16_t *addr, uint32_t symbol_id) { return *addr; }
extern "C" uint32_t tm_read_i4(volatile uint32_t *addr, uint32_t symbol_id) { return *addr; }
extern "C" uint64_t tm_read_i8(volatile uint64_t *addr, uint32_t symbol_id) { return *addr; }
extern "C" float tm_read_f4(volatile float *addr, uint32_t symbol_id) { return *addr; }
extern "C" double tm_read_f8(volatile double *addr, uint32_t symbol_id) { return *addr; }
extern "C" void *tm_read_ptr(volatile void **addr, uint32_t symbol_id) { return (void*)*addr; }

extern "C" void *tm_read_z(volatile uint8_t *src, uint64_t len, uint32_t symbol_id) {
    if (len < TM_BUFFER_SIZE) {
        memcpy((void*)tm_tx_buffer, (const void*)src, len);
        return tm_tx_buffer;
    }
    void *buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

extern "C" void tm_write_i1(volatile uint8_t *addr, uint8_t val, uint32_t symbol_id) { *addr = val; }
extern "C" void tm_write_i2(volatile uint16_t *addr, uint16_t val, uint32_t symbol_id) { *addr = val; }
extern "C" void tm_write_i4(volatile uint32_t *addr, uint32_t val, uint32_t symbol_id) { *addr = val; }
extern "C" void tm_write_i8(volatile uint64_t *addr, uint64_t val, uint32_t symbol_id) { *addr = val; }
extern "C" void tm_write_f4(volatile float *addr, float val, uint32_t symbol_id) { *addr = val; }
extern "C" void tm_write_f8(volatile double *addr, double val, uint32_t symbol_id) { *addr = val; }
extern "C" void tm_write_ptr(volatile void **addr, void *val, uint32_t symbol_id) { *addr = val; }

extern "C" void tm_write_z(volatile uint8_t *dst, volatile uint8_t *src, uint64_t len, uint32_t symbol_id) {
    memcpy((void*)dst, (const void*)src, len);
}

extern "C" void tm_memset(volatile uint8_t *addr, uint8_t val, uint64_t len, uint32_t symbol_id) {
    memset((void*)addr, val, len);
}

extern "C" void consume_ptr(volatile void *ptr) { (void)ptr; }