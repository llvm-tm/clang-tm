/**
 * TM Runtime Stub - provides symbols needed by instrumented code
 */

#include <cstdint>
#include <csetjmp>

thread_local int32_t __tm_nested_call_counter = 0;
thread_local sigjmp_buf __tm_jmpbuf;

extern "C" void tm_setjmp_helper() {
    // Required by instrumented code with setjmp
}

extern "C" void tm_longjmp_helper(int val) {
    // Required by instrumented code with setjmp  
    siglongjmp(__tm_jmpbuf, val);
}