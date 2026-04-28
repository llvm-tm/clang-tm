// Runtime with symbol table support
#include <cstdint>
#include <cstdio>
#include <cstring>

#define TM_BUFFER_SIZE 4096

thread_local uint8_t tm_tx_buffer[TM_BUFFER_SIZE];

// Symbol tables (filled in by plugin)
extern "C" const char* tm_symbol_names[];
extern "C" void* tm_symbol_addresses[];
extern "C" const uint32_t tm_symbol_count;

// Get symbol ID from address (binary search with offset support)
extern "C" uint32_t tm_get_symbol_id(void* addr) {
    uintptr_t target = reinterpret_cast<uintptr_t>(addr);
    uintptr_t best_id = 0xFFFFFFFF;
    uintptr_t best_offset = 0xFFFFFFFFFFFFFFFF;
    
    for (uint32_t i = 0; i < tm_symbol_count; i++) {
        uintptr_t sym_addr = reinterpret_cast<uintptr_t>(tm_symbol_addresses[i]);
        if (target >= sym_addr) {
            uintptr_t offset = target - sym_addr;
            if (offset < best_offset) {
                best_offset = offset;
                best_id = i;
            }
        }
    }
    return static_cast<uint32_t>(best_id);
}

// Get symbol name from ID
static const char* get_symbol_name(uint32_t id) {
    if (id == 0xFFFFFFFF || id >= tm_symbol_count) {
        return "<unknown>";
    }
    return tm_symbol_names[id];
}

extern "C" void tm_init() {
    printf("tm_init (symbol_count=%u)\n", tm_symbol_count);
    for (uint32_t i = 0; i < tm_symbol_count; i++) {
        printf("  symbol[%u]: %s @ %p\n", i, tm_symbol_names[i] ? tm_symbol_names[i] : "<null>", tm_symbol_addresses[i]);
    }
    fflush(stdout);
}

extern "C" void tm_exit() {
    printf("tm_exit\n");
    fflush(stdout);
}

extern "C" void tm_init_thread() {
    printf("tm_init_thread\n");
    fflush(stdout);
}

extern "C" void tm_exit_thread() {
    printf("tm_exit_thread\n");
    fflush(stdout);
}

extern "C" void tm_begin() {
    printf("tm_begin\n");
    fflush(stdout);
}

extern "C" void tm_end() {
    printf("tm_end\n");
    fflush(stdout);
}

extern "C" uint8_t tm_read_i1(uint8_t *addr) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_read_i1(%s[%p]) = %d\n", get_symbol_name(id), (void*)addr, *addr);
    return *addr;
}

extern "C" uint16_t tm_read_i2(uint16_t *addr) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_read_i2(%s[%p]) = %d\n", get_symbol_name(id), (void*)addr, *addr);
    return *addr;
}

extern "C" uint32_t tm_read_i4(uint32_t *addr) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_read_i4(%s[%p]) = %d\n", get_symbol_name(id), (void*)addr, *addr);
    return *addr;
}

extern "C" uint64_t tm_read_i8(uint64_t *addr) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_read_i8(%s[%p]) = %lld\n", get_symbol_name(id), (void*)addr, (long long)*addr);
    return *addr;
}

extern "C" float tm_read_f4(float *addr) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_read_f4(%s[%p]) = %f\n", get_symbol_name(id), (void*)addr, *addr);
    return *addr;
}

extern "C" double tm_read_f8(double *addr) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_read_f8(%s[%p]) = %lf\n", get_symbol_name(id), (void*)addr, *addr);
    return *addr;
}

extern "C" void *tm_read_ptr(void **addr) {
    uint32_t id = tm_get_symbol_id(addr);
    void *val = *addr;
    printf("tm_read_ptr(%s[%p]) = %p\n", get_symbol_name(id), (void*)addr, val);
    return val;
}

extern "C" void *tm_read_z(uint8_t *src, uint64_t len) {
    uint32_t id = tm_get_symbol_id(src);
    if (len < TM_BUFFER_SIZE) {
        memcpy(tm_tx_buffer, src, len);
    }
    printf("tm_read_z(%s[%p], %llu)\n", get_symbol_name(id), (void*)src, (unsigned long long)len);
    return tm_tx_buffer;
}

extern "C" void tm_write_i1(uint8_t *addr, uint8_t val) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_write_i1(%s[%p], %d)\n", get_symbol_name(id), (void*)addr, val);
    *addr = val;
}

extern "C" void tm_write_i2(uint16_t *addr, uint16_t val) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_write_i2(%s[%p], %d)\n", get_symbol_name(id), (void*)addr, val);
    *addr = val;
}

extern "C" void tm_write_i4(uint32_t *addr, uint32_t val) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_write_i4(%s[%p], %d)\n", get_symbol_name(id), (void*)addr, val);
    *addr = val;
}

extern "C" void tm_write_i8(uint64_t *addr, uint64_t val) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_write_i8(%s[%p], %lld)\n", get_symbol_name(id), (void*)addr, (long long)val);
    *addr = val;
}

extern "C" void tm_write_f4(float *addr, float val) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_write_f4(%s[%p], %f)\n", get_symbol_name(id), (void*)addr, val);
    *addr = val;
}

extern "C" void tm_write_f8(double *addr, double val) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_write_f8(%s[%p], %lf)\n", get_symbol_name(id), (void*)addr, val);
    *addr = val;
}

extern "C" void tm_write_ptr(void **addr, void *val) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_write_ptr(%s[%p], %p)\n", get_symbol_name(id), (void*)addr, val);
    *addr = val;
}

extern "C" void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len) {
    uint32_t id = tm_get_symbol_id(dst);
    printf("tm_write_z(%s[%p], %p, %llu)\n", get_symbol_name(id), (void*)dst, (void*)src, (unsigned long long)len);
    memcpy(dst, src, len);
}

extern "C" void tm_memset(uint8_t *addr, uint8_t val, uint64_t len) {
    uint32_t id = tm_get_symbol_id(addr);
    printf("tm_memset(%s[%p], %d, %llu)\n", get_symbol_name(id), (void*)addr, val, (unsigned long long)len);
    memset(addr, val, len);
}