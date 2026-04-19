/**
 * TM Runtime Provider - provides all symbols expected by instrumented code
 */

#include <cstdint>
#include <csetjmp>

// Thread-local storage using simple non-thread-local (all threads share - for runtime compat)
__attribute__((visibility("default")))
int32_t tm_nested_call_counter = 0;

__attribute__((visibility("default")))
int32_t __tm_nested_call_counter = 0;

__attribute__((visibility("default")))
sigjmp_buf tm_jmpbuf;

__attribute__((visibility("default")))
sigjmp_buf __tm_jmpbuf;

int __tm_dummy_for_linker = 0;