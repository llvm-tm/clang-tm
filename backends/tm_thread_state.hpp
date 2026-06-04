#ifndef TM_THREAD_STATE_HPP
#define TM_THREAD_STATE_HPP

#include <cstdint>

// Per-thread TM runtime state stored in the TM address space.
// The LLVM plugin accesses these fields via tm_get_thread_state() + GEP,
// using hardcoded offsets that must match this struct exactly.
//
// Plugin offset constants (tm_instrument_helpers.hpp):
//   COUNTER_OFFSET = 0  (nested_call_counter)
//   JMPRET_OFFSET  = 4  (longjmp_ret)
struct TMThreadState {
    int32_t nested_call_counter;
    int32_t longjmp_ret;
};

extern "C" TMThreadState *tm_get_thread_state();

#endif // TM_THREAD_STATE_HPP
