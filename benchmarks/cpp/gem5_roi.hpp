// gem5 region-of-interest (ROI) markers via m5 pseudo-instructions.
//
// Active only when compiled with -DGEM5_M5OPS (x86-64 SE-mode builds).
// The sequence `.byte 0x0F,0x04; .word <func>` is the same encoding used
// by gem5's util/m5 (src/abi/x86/m5op.S); on real hardware it is an
// undefined opcode (#UD), so never enable GEM5_M5OPS for native builds.
//
// Function numbers from gem5/include/gem5/asm/generic/m5ops.h:
//   M5OP_EXIT = 0x21, M5OP_RESET_STATS = 0x40, M5OP_DUMP_STATS = 0x41,
//   M5OP_WORK_BEGIN = 0x5a, M5OP_WORK_END = 0x5b
//
// In gem5 the reset/dump pseudo-instructions reset/dump simulator
// statistics, so stats.txt covers only the code between
// ROI_RESET_STATS/ROI_DUMP_STATS and no OS shutdown is simulated.
//
// Checkpoint-and-continue (ch16): compiling with -DGEM5_CKPT (in addition
// to -DGEM5_M5OPS) emits gem5 "work item" annotations (m5_work_begin /
// m5_work_end) around the ROI.  gem5's System params
// `work_begin_ckpt_count`/`work_end_ckpt_count` turn these into a
// `simulate exit reason "checkpoint"` at the ROI start — the point right
// after the stats reset where the process is fully initialised — so a
// save-phase config can call `save_checkpoint` there, and a restore-phase
// config can resume from that checkpoint without re-running init.  See
// gem5_sim/configs/x86-se-bank-checkpoint.py and x86-se-bank-restore.py.
// When GEM5_CKPT is *not* defined these macros expand to the plain
// reset/dump/exit markers (no checkpointing).
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

static inline void work_begin(uint64_t workid, uint64_t threadid) {
    __asm__ volatile(".byte 0x0F, 0x04; .word 0x5a"
                     :
                     : "D"(workid), "S"(threadid)
                     : "memory", "cc");
}

static inline void work_end(uint64_t workid, uint64_t threadid) {
    __asm__ volatile(".byte 0x0F, 0x04; .word 0x5b"
                     :
                     : "D"(workid), "S"(threadid)
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

#ifdef GEM5_CKPT
#define ROI_CKPT_BEGIN() ::gem5_roi::work_begin(0, 0)
#define ROI_CKPT_END() ::gem5_roi::work_end(0, 0)
#else
#define ROI_CKPT_BEGIN() ((void)0)
#define ROI_CKPT_END() ((void)0)
#endif

#define ROI_RESET_STATS() ::gem5_roi::reset_stats(0, 0)
#define ROI_DUMP_STATS() ::gem5_roi::dump_stats(0, 0)
#define ROI_EXIT(code) ::gem5_roi::sim_exit(0, (uint64_t)(code))

#else // !GEM5_M5OPS

#define ROI_RESET_STATS() ((void)0)
#define ROI_DUMP_STATS() ((void)0)
#define ROI_EXIT(code) ((void)0)
#define ROI_CKPT_BEGIN() ((void)0)
#define ROI_CKPT_END() ((void)0)

#endif // GEM5_M5OPS
