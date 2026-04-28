// This is what the plugin produces for memtest.cpp
// Instrumented version of: test/memtest.cpp

#include <cstring>
#include <cstdint>
#include <csetjmp>
#include <cstdio>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

// TM globals
TM char tm_buf1[16];
TM char tm_buf2[16];
TM int32_t tm_int_buf[4];
char non_tm_buf[16];

// Thread-local state
__thread int32_t tm_nested_call_counter = 0;
__thread unsigned char tm_jmpbuf[256];
__thread int32_t tm_jmpbuf_ret = 0;
__thread uint8_t is_tm_init_thread_ready = 0;

// Runtime hooks
void tm_init() { printf("tm_init\n"); }
void tm_exit() { printf("tm_exit\n"); }
void tm_init_thread() {
    if (is_tm_init_thread_ready == 0) {
        printf("tm_init_thread\n");
        is_tm_init_thread_ready = 1;
    }
}
void tm_exit_thread() {
    if (is_tm_init_thread_ready == 1) {
        printf("tm_exit_thread\n");
        is_tm_init_thread_ready = 0;
    }
}

void tm_begin() {
    if (tm_jmpbuf_ret == 0) { // not an abort
        printf("tm_nested_call_counter=%d  --  ", tm_nested_call_counter);
        if (tm_nested_call_counter == 1) {
            printf("tm_begin outer\n");
        } else {
            printf("tm_begin nested %d\n", tm_nested_call_counter);
        }
    }
}
void tm_end() {
    printf("tm_nested_call_counter=%d  --  ", tm_nested_call_counter);
    if (tm_nested_call_counter == 1) {
        printf("tm_end outer\n");
    } else {
        printf("tm_end nested %d\n", tm_nested_call_counter);
    }
}

// Memory operation instrumentation
void tm_memset(void* dst, uint8_t value, size_t sz) {
    memset(dst, value, sz);
    printf("tm_memset(%p, %u, %zu)\n", dst, value, sz);
}
void tm_write_z(void* dst, void* src, size_t sz) {
    memcpy(dst, src, sz);
    printf("tm_write_z(%p, %p, %zu)\n", dst, src, sz);
}
void* tm_read_z(void* src, size_t sz) {
    static char buffer[1024];
    memcpy(buffer, src, sz);
    printf("tm_read_z(%p, %zu) = %p\n", src, sz, buffer);
    return buffer;
}

void tm_memops() {
    tm_nested_call_counter++;

    // Entry handling
    if (tm_nested_call_counter == 1) {
        // OUTER path
        tm_jmpbuf_ret = sigsetjmp(*(sigjmp_buf*)tm_jmpbuf, 0);
        if (tm_jmpbuf_ret == 0) {
            tm_nested_call_counter = 1;
        }
        tm_begin();
    }
    // NESTED path: NO tm_begin() - only outermost calls tm_begin()

    // Original function body with TM accesses instrumented:
    tm_memset(tm_buf1, 0xAB, sizeof(tm_buf1));
    tm_buf1[0] = 0xCD;
    tm_write_z(tm_buf2, tm_buf1, sizeof(tm_buf1));
    {
        void* src = tm_read_z(tm_buf1, sizeof(tm_buf1));
        memcpy(non_tm_buf, src, sizeof(tm_buf1));
    }
    tm_memset(tm_int_buf, 0, sizeof(tm_int_buf));

    // Exit handling
    if (tm_nested_call_counter == 1) {
        tm_end();
    }
tm_nested_call_counter--;
}

int main() {
    tm_init();

    if (is_tm_init_thread_ready == 0) {
        tm_init_thread();
        is_tm_init_thread_ready = 1;
    }

    tm_memops();

    if (is_tm_init_thread_ready == 1) {
        tm_exit_thread();
        is_tm_init_thread_ready = 0;
    }
    
    tm_exit();
    return 0;
}
    
    // memset(tm_int_buf, 0, sizeof(tm_int_buf));
    tm_memset(tm_int_buf, 0, sizeof(tm_int_buf));

    // Exit handling
    if (tm_nested_call_counter == 1) {
        tm_end();
    }
    tm_nested_call_counter--;
}

int main() {
    tm_init();

    if (is_tm_init_thread_ready == 0) {
        tm_init_thread();
        is_tm_init_thread_ready = 1;
    }

    tm_memops();

    if (is_tm_init_thread_ready == 1) {
        tm_exit_thread();
        is_tm_init_thread_ready = 0;
    }
    
    tm_exit();
    return 0;
}