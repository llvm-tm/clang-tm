#pragma once

#include <cstdint>
#include <cstddef>

// ── PR-STM Configuration ──────────────────────────────────────────

#define PR_STM_WARP_SIZE       32
#define PR_STM_MAX_READS       64      // max reads per thread per tx
#define PR_STM_MAX_WRITES      32      // max writes per thread per tx
#define PR_STM_LOCKTABLE_SIZE  (1 << 20)  // 1M entries

// ── Lock word encoding ────────────────────────────────────────────
// 32-bit lock word: [priority:8 | version:23 | locked:1]
//   bit 31..24: priority (0 = free, 1..255 = warp priority)
//   bit 23..1:  version (global clock value)
//   bit 0:      locked flag

#define PR_STM_PRIORITY_SHIFT  24
#define PR_STM_VERSION_SHIFT   1
#define PR_STM_LOCKED_BIT      (1u << 0)
#define PR_STM_PRIORITY_MASK   (0xFFu << PR_STM_PRIORITY_SHIFT)
#define PR_STM_VERSION_MASK    ((1u << PR_STM_PRIORITY_SHIFT) - 2)

inline uint32_t pr_stm_make_entry(uint8_t priority, uint32_t version, int locked) {
    return ((uint32_t)priority << PR_STM_PRIORITY_SHIFT) |
           (version << PR_STM_VERSION_SHIFT) |
           (locked ? PR_STM_LOCKED_BIT : 0u);
}

inline uint8_t  pr_stm_get_priority(uint32_t entry) { return (uint8_t)(entry >> PR_STM_PRIORITY_SHIFT); }
inline uint32_t pr_stm_get_version(uint32_t entry)  { return (entry >> PR_STM_VERSION_SHIFT) & (PR_STM_VERSION_MASK >> PR_STM_VERSION_SHIFT); }
inline int      pr_stm_is_locked(uint32_t entry)    { return (int)(entry & PR_STM_LOCKED_BIT); }

// ── Host API (CPU side, matches TMRealHooks pattern) ──────────────

// Lifecycle
void gpu_tm_init(void);
void gpu_tm_exit(void);
void gpu_tm_init_thread(void);
void gpu_tm_exit_thread(void);
void gpu_tm_begin(void);
void gpu_tm_end(void);

// Allocation (unified memory for host-device sharing)
void *gpu_tm_malloc(size_t sz);
void  gpu_tm_free(void *p);
void *gpu_tm_calloc(size_t nmemb, size_t sz);
void *gpu_tm_realloc(void *p, size_t sz);

// Reads
uint8_t  gpu_tm_read_i1(uint8_t *addr);
uint16_t gpu_tm_read_i2(uint16_t *addr);
uint32_t gpu_tm_read_i4(uint32_t *addr);
uint64_t gpu_tm_read_i8(uint64_t *addr);
float    gpu_tm_read_f4(float *addr);
double   gpu_tm_read_f8(double *addr);
void    *gpu_tm_read_ptr(void **addr);

// Writes
void gpu_tm_write_i1(uint8_t *addr, uint8_t val);
void gpu_tm_write_i2(uint16_t *addr, uint16_t val);
void gpu_tm_write_i4(uint32_t *addr, uint32_t val);
void gpu_tm_write_i8(uint64_t *addr, int64_t val);
void gpu_tm_write_f4(float *addr, float val);
void gpu_tm_write_f8(double *addr, double val);
void gpu_tm_write_ptr(void **addr, void *val);

// ── GPU kernel launch API ─────────────────────────────────────────
// Launch a persistent PR-STM kernel that executes transactions
// on GPU. Each block = one warp, each thread = one lane.

typedef void (*pr_stm_tx_body_t)(int lane_id, int warp_id,
                                  void *data, void *shared_scratch);

int gpu_pr_stm_launch(int num_warps,
                       pr_stm_tx_body_t tx_body,
                       void *tx_data);

// ── CPU fallback (no GPU required) ────────────────────────────────
// Emulates PR-STM using std::thread. Each thread simulates one
// GPU thread (lane) within a warp. A std::barrier or spin-loop
// enforces phase lockstep matching the SIMT model.

void cpu_pr_stm_begin(int warp_id, int lane_id);
void cpu_pr_stm_read(int warp_id, int lane_id, uint32_t *addr);
void cpu_pr_stm_write(int warp_id, int lane_id, uint32_t *addr, uint32_t val);
int  cpu_pr_stm_commit(int warp_id, int lane_id);
void cpu_pr_stm_abort(int warp_id, int lane_id);
void cpu_pr_stm_end(int warp_id, int lane_id);

// High-level CPU emulation (one-shot: runs all threads to completion)
void cpu_pr_stm_emulate(int num_warps, int warp_size,
                         int num_addrs, int reads_per_thread,
                         int writes_per_thread);
