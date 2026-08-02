#pragma once
#include <cstdint>
#include <cstddef>

#define GPU_GPUTX_WARP_SIZE      32
#define GPU_GPUTX_MAX_READS      64
#define GPU_GPUTX_MAX_WRITES     32
#define GPU_GPUTX_LOCKTABLE_SIZE (1 << 20)

#define GPU_GPUTX_PRIORITY_SHIFT 24
#define GPU_GPUTX_LOCKED_BIT     (1u << 0)

#if defined(__CUDACC__) || defined(__HIPCC__)
  #define GPU_GPUTX_DEVICE __device__
#else
  #define GPU_GPUTX_DEVICE
#endif

GPU_GPUTX_DEVICE
inline uint32_t gpu_gputx_make_entry(uint32_t priority, uint32_t version) {
    return (priority << GPU_GPUTX_PRIORITY_SHIFT) | (version << 1) | GPU_GPUTX_LOCKED_BIT;
}

GPU_GPUTX_DEVICE
inline uint32_t gpu_gputx_get_priority(uint32_t entry) {
    return entry >> GPU_GPUTX_PRIORITY_SHIFT;
}

GPU_GPUTX_DEVICE
inline uint32_t gpu_gputx_get_version(uint32_t entry) {
    return (entry >> 1) & ((1u << GPU_GPUTX_PRIORITY_SHIFT) - 1);
}

GPU_GPUTX_DEVICE
inline int gpu_gputx_is_locked(uint32_t entry) {
    return (int)(entry & GPU_GPUTX_LOCKED_BIT);
}

void gpu_gputx_init(void);
void gpu_gputx_exit(void);
void gpu_gputx_init_thread(void);
void gpu_gputx_exit_thread(void);
void gpu_gputx_begin(void);
void gpu_gputx_end(void);

void *gpu_gputx_malloc(size_t sz);
void  gpu_gputx_free(void *p);
void *gpu_gputx_calloc(size_t nmemb, size_t sz);
void *gpu_gputx_realloc(void *p, size_t sz);

uint8_t  gpu_gputx_read_i1(uint8_t *addr);
uint16_t gpu_gputx_read_i2(uint16_t *addr);
uint32_t gpu_gputx_read_i4(uint32_t *addr);
uint64_t gpu_gputx_read_i8(uint64_t *addr);
float    gpu_gputx_read_f4(float *addr);
double   gpu_gputx_read_f8(double *addr);
void    *gpu_gputx_read_ptr(void **addr);

void gpu_gputx_write_i1(uint8_t *addr, uint8_t val);
void gpu_gputx_write_i2(uint16_t *addr, uint16_t val);
void gpu_gputx_write_i4(uint32_t *addr, uint32_t val);
void gpu_gputx_write_i8(uint64_t *addr, int64_t val);
void gpu_gputx_write_f4(float *addr, float val);
void gpu_gputx_write_f8(double *addr, double val);
void gpu_gputx_write_ptr(void **addr, void *val);

int gpu_gputx_launch(int num_warps);
