#pragma once
#include <cstdint>
#include <cstddef>

// ── GUST (Scalable Multi-Version Concurrency Control for GPUs) ──
// Nunes, Castro, Romano (IST / INESC-ID).  MVCC with Versioned Boxes
// (VBoxes), a global timestamp (GTS), and a bounded circular Commit
// Log (CL).  Commit timestamps come from an AtomicINC (no CAS).
// Validation is hybrid CCT + MRV.

#define GPU_GUST_WARP_SIZE      32
#define GPU_GUST_MAX_READS      64
#define GPU_GUST_MAX_WRITES     32

// VBox version-list depth (circular).  Must be ≥ the maximum number of
// committed versions a snapshot reader may need to walk back.
#define GPU_GUST_VBOX_DEPTH     8

// Commit Log capacity.  Must be ≫ maximum number of concurrent update
// transactions (num_warps * WARP_SIZE).  Bounded circular buffer; slots
// are indexed by (CTS % CL_SIZE).  Power of two for cheap masking.
#define GPU_GUST_CL_SIZE        (1 << 16)
#define GPU_GUST_CL_MASK        (GPU_GUST_CL_SIZE - 1)

// Commit Log entry states
enum {
    GPU_GUST_CL_FREE     = 0,   // slot unused
    GPU_GUST_CL_PENDING  = 1,   // transaction in commit, not yet final
    GPU_GUST_CL_COMMITTED = 2,  // write-back finished, version published
    GPU_GUST_CL_ABORTED  = 3,   // transaction aborted
};

// ── Versioned Box (device struct) ───────────────────────────────
// Each shared data item maps to one VBox holding the most recent
// committed versions as a circular array, newest at (head-1) % DEPTH.
// `head` is an atomic, monotonic append counter (slot = head % DEPTH).
// Version 0 is a sentinel meaning "no committed version yet".
struct GUSTVBox {
    uint64_t versions[GPU_GUST_VBOX_DEPTH];   // commit timestamps
    uint32_t values[GPU_GUST_VBOX_DEPTH];     // committed values
    uint32_t head;                            // atomic append counter
    uint32_t _pad;                            // 8-byte align
};

// ── Commit Log entry (device struct) ────────────────────────────
// One entry per reserved commit timestamp.  write-set is stored so that
// concurrent transactions can perform CCT validation against it.
struct GUSTCLEntry {
    uint32_t state;                            // GUST_CL_*
    uint32_t num_writes;
    uint32_t write_addrs[GPU_GUST_MAX_WRITES];
    uint32_t write_vals[GPU_GUST_MAX_WRITES];
};

#if defined(__CUDACC__) || defined(__HIPCC__)
  #define GPU_GUST_DEVICE __device__
#else
  #define GPU_GUST_DEVICE
#endif

// ── Host TM API (mirrors gpu_gacco) ─────────────────────────────
void gpu_gust_init(void);
void gpu_gust_exit(void);
void gpu_gust_init_thread(void);
void gpu_gust_exit_thread(void);
void gpu_gust_begin(void);
void gpu_gust_end(void);

void *gpu_gust_malloc(size_t sz);
void  gpu_gust_free(void *p);
void *gpu_gust_calloc(size_t nmemb, size_t sz);
void *gpu_gust_realloc(void *p, size_t sz);

uint8_t  gpu_gust_read_i1(uint8_t *addr);
uint16_t gpu_gust_read_i2(uint16_t *addr);
uint32_t gpu_gust_read_i4(uint32_t *addr);
uint64_t gpu_gust_read_i8(uint64_t *addr);
float    gpu_gust_read_f4(float *addr);
double   gpu_gust_read_f8(double *addr);
void    *gpu_gust_read_ptr(void **addr);

void gpu_gust_write_i1(uint8_t *addr, uint8_t val);
void gpu_gust_write_i2(uint16_t *addr, uint16_t val);
void gpu_gust_write_i4(uint32_t *addr, uint32_t val);
void gpu_gust_write_i8(uint64_t *addr, int64_t val);
void gpu_gust_write_f4(float *addr, float val);
void gpu_gust_write_f8(double *addr, double val);
void gpu_gust_write_ptr(void **addr, void *val);

int gpu_gust_launch(int num_warps);
