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
	int32_t nested_call_counter; // offset 0
	int32_t longjmp_ret;         // offset 4
	// abort/retry budget counter for the plugin's -tm-max-retries bound;
	// maintained solely by pass-injected code (offset 8)
	int32_t retry_count;
};

#endif // TM_THREAD_STATE_HPP
