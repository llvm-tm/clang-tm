// gem5 region-of-interest (ROI) markers via m5 pseudo-instructions.
//
// Active only when compiled with -DGEM5_M5OPS (x86-64 SE-mode builds).
// The sequence `.byte 0x0F,0x04; .word <func>` is the same encoding used
// by gem5's util/m5 (src/abi/x86/m5op.S); on real hardware it is an
// undefined opcode (#UD), so never enable GEM5_M5OPS for native builds.
//
// Function numbers from gem5/include/gem5/asm/generic/m5ops.h:
//   M5OP_EXIT = 0x21, M5OP_RESET_STATS = 0x40, M5OP_DUMP_STATS = 0x41
//
// In gem5 the pseudo-inst resets/dumps simulator statistics (or ends the
// simulation), so stats.txt covers only the code between
// ROI_RESET_STATS/ROI_DUMP_STATS and no OS shutdown is simulated.
#pragma once

#ifdef GEM5_M5OPS

#include <cstdint>

namespace gem5_roi {

// Arguments are passed in rdi/rsi per the SysV ABI (as in util/m5).
static inline void reset_stats(uint64_t delay, uint64_t period) {
    __asm__ volatile(".byte 0x0F, 0x04; .word 0x40"
                     :
                     : "D"(delay), "S"(period)
                     : "memory", "cc");
}

static inline void dump_stats(uint64_t delay, uint64_t period) {
    __asm__ volatile(".byte 0x0F, 0x04; .word 0x41"
                     :
                     : "D"(delay), "S"(period)
                     : "memory", "cc");
}

// End the simulation immediately from inside the guest (FS mode: skips
// the OS shutdown entirely — the ROI is over, nothing else matters).
static inline void sim_exit(uint64_t delay, uint64_t code) {
    __asm__ volatile(".byte 0x0F, 0x04; .word 0x21"
                     :
                     : "D"(delay), "S"(code)
                     : "memory", "cc");
}

} // namespace gem5_roi

#define ROI_RESET_STATS() ::gem5_roi::reset_stats(0, 0)
#define ROI_DUMP_STATS() ::gem5_roi::dump_stats(0, 0)
#define ROI_EXIT(code) ::gem5_roi::sim_exit(0, (uint64_t)(code))

#else // !GEM5_M5OPS

#define ROI_RESET_STATS() ((void)0)
#define ROI_DUMP_STATS() ((void)0)
#define ROI_EXIT(code) ((void)0)

#endif // GEM5_M5OPS
