/**
 * TM Runtime for nested.cpp test
 * Uses regular globals for simplicity
 */

#include <cstdint>
#include <cstdio>
#include <csetjmp>
#include <mutex>

int8_t tm_is_init_ready = 0;
int8_t tm_is_init_thread_ready = 0;
__thread int32_t tm_setjmp_ret = 0;
__thread int32_t tm_longjmp_ret;
__thread int32_t tm_nested_call_counter;
__thread int32_t __tm_nested_call_counter = 0;
__thread sigjmp_buf tm_jmpbuf;
__thread sigjmp_buf __tm_jmpbuf;

extern "C" {

int tm_setjmp() {
    return 0;
}

void tm_set_jmpbuf(void *buf) { }

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) {
}

}

extern "C" void tm_init() {
    printf("tm_init\n");
    tm_is_init_ready = 1;
    fflush(stdout);
}

extern "C" void tm_exit() {
    printf("tm_exit\n");
    fflush(stdout);
}

extern "C" void tm_init_thread() {
    printf("tm_init_thread (was %d)\n", (int)tm_is_init_thread_ready);
    tm_is_init_thread_ready = 1;
    fflush(stdout);
}

extern "C" void tm_exit_thread() {
    printf("tm_exit_thread\n");
    tm_is_init_thread_ready = 0;
    fflush(stdout);
}

static std::recursive_mutex g_serialize_mutex;

extern "C" void tm_serialize_lock() { g_serialize_mutex.lock(); }

extern "C" void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

extern "C" void tm_begin() {
    printf("tm_begin: setjmp_ret = %d\n", tm_setjmp_ret);
}

extern "C" void tm_end() {
}

extern "C" int8_t tm_read_i1(int8_t* addr, int32_t symbol_id) {
    (void)symbol_id;
    return *addr;
}

extern "C" void tm_write_i1(int8_t* addr, int8_t val, int32_t symbol_id) {
    (void)symbol_id;
    *addr = val;
}

extern "C" int32_t tm_read_i4(int32_t* addr, int32_t symbol_id) {
    (void)symbol_id;
    return *addr;
}

extern "C" void tm_write_i4(int32_t* addr, int32_t val, int32_t symbol_id) {
    (void)symbol_id;
    *addr = val;
}
