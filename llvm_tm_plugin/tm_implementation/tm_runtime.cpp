#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <csetjmp>

#define TM_BUFFER_SIZE 1024

__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter = 0;
__thread uint8_t tm_buffer[TM_BUFFER_SIZE] = {0};

extern "C" void tm_init() {
    printf("tm_init\n");
}

extern "C" void tm_exit() {
    printf("tm_exit\n");
}

extern "C" void tm_init_thread() {
    printf("tm_init_thread\n");
}

extern "C" void tm_exit_thread() {
    printf("tm_exit_thread\n");
}

extern "C" void tm_begin() {
    if (tm_nested_call_counter == 0) {
        int ret = setjmp(tm_jmpbuf);
        (void)ret;
        printf("tm_begin outer\n");
    } else {
        printf("tm_begin nested %d\n", tm_nested_call_counter);
    }
    tm_nested_call_counter++;
}

extern "C" void tm_end() {
    if (tm_nested_call_counter <= 0) {
        printf("tm_end underflow\n");
        return;
    }
    tm_nested_call_counter--;
    if (tm_nested_call_counter == 0) {
        printf("tm_end outer\n");
    } else {
        printf("tm_end nested %d\n", tm_nested_call_counter + 1);
    }
}

extern "C" int8_t tm_read_i1(void* addr) {
    int8_t val = *(int8_t*)addr;
    printf("tm_read_i1(%p) = %d\n", addr, val);
    return val;
}

extern "C" int16_t tm_read_i2(void* addr) {
    int16_t val = *(int16_t*)addr;
    printf("tm_read_i2(%p) = %d\n", addr, val);
    return val;
}

extern "C" int32_t tm_read_i4(void* addr) {
    int32_t val = *(int32_t*)addr;
    printf("tm_read_i4(%p) = %d\n", addr, val);
    return val;
}

extern "C" int64_t tm_read_i8(void* addr) {
    int64_t val = *(int64_t*)addr;
    printf("tm_read_i8(%p) = %lld\n", addr, val);
    return val;
}

extern "C" float tm_read_f4(void* addr) {
    float val = *(float*)addr;
    printf("tm_read_f4(%p) = %f\n", addr, val);
    return val;
}

extern "C" double tm_read_f8(void* addr) {
    double val = *(double*)addr;
    printf("tm_read_f8(%p) = %lf\n", addr, val);
    return val;
}

extern "C" void* tm_read_ptr(void* addr) {
    void* val = *(void**)addr;
    printf("tm_read_ptr(%p) = %p\n", addr, val);
    return val;
}

extern "C" void* tm_read_z(void* src, size_t sz) {
    assert(sz < TM_BUFFER_SIZE);
    memcpy(tm_buffer, src, sz);
    printf("tm_read_z(%p, %zu) = %p\n", src, sz, (void*)tm_buffer);
    return tm_buffer;
}

extern "C" void tm_write_i1(void* addr, uint8_t val) {
    printf("tm_write_i1(%p, %u)\n", addr, val);
    *(uint8_t*)addr = val;
}

extern "C" void tm_write_i2(void* addr, int16_t val) {
    printf("tm_write_i2(%p, %hd)\n", addr, val);
    *(int16_t*)addr = val;
}

extern "C" void tm_write_i4(void* addr, int32_t val) {
    printf("tm_write_i4(%p, %d)\n", addr, val);
    *(int32_t*)addr = val;
}

extern "C" void tm_write_i8(void* addr, int64_t val) {
    printf("tm_write_i8(%p, %lld)\n", addr, val);
    *(int64_t*)addr = val;
}

extern "C" void tm_write_f4(void* addr, float val) {
    printf("tm_write_f4(%p, %f)\n", addr, val);
    *(float*)addr = val;
}

extern "C" void tm_write_f8(void* addr, double val) {
    printf("tm_write_f8(%p, %lf)\n", addr, val);
    *(double*)addr = val;
}

extern "C" void tm_write_ptr(void* addr, void* val) {
    printf("tm_write_ptr(%p, %p)\n", addr, val);
    *(void**)addr = val;
}

extern "C" void tm_write_z(void* dst, void* src, size_t sz) {
    assert(sz < TM_BUFFER_SIZE);
    memcpy(dst, src, sz);
    printf("tm_write_z(%p, %p, %zu)\n", dst, src, sz);
}

extern "C" void tm_memset(void* dst, uint8_t value, size_t sz) {
    assert(sz < TM_BUFFER_SIZE);
    memset(dst, value, sz);
    printf("tm_memset(%p, %u, %zu)\n", dst, value, sz);
}

extern "C" void consume_ptr(volatile void* ptr) {
    (void)ptr;
}
