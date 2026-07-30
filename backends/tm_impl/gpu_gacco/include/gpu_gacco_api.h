#pragma once
#include <cstdint>
#include <cstddef>

#define GPU_GACCO_WARP_SIZE      32
#define GPU_GACCO_MAX_READS      64
#define GPU_GACCO_MAX_WRITES     32
#define GPU_GACCO_LOCKTABLE_SIZE (1 << 20)

#define GPU_GACCO_PRIORITY_SHIFT 24
#define GPU_GACCO_LOCKED_BIT     (1u << 0)

#if defined(__CUDACC__) || defined(__HIPCC__)
  #define GPU_GACCO_DEVICE __device__
#else
  #define GPU_GACCO_DEVICE
#endif

GPU_GACCO_DEVICE
inline uint32_t gpu_gacco_make_entry(uint32_t holder, uint32_t version) {
    return (holder << GPU_GACCO_PRIORITY_SHIFT) | version;
}

GPU_GACCO_DEVICE
inline uint32_t gpu_gacco_get_holder(uint32_t entry) {
    return entry >> GPU_GACCO_PRIORITY_SHIFT;
}

GPU_GACCO_DEVICE
inline uint32_t gpu_gacco_get_version(uint32_t entry) {
    return entry & ((1u << GPU_GACCO_PRIORITY_SHIFT) - 1);
}

GPU_GACCO_DEVICE
inline int gpu_gacco_is_locked(uint32_t entry) {
    return (entry & GPU_GACCO_LOCKED_BIT) ? 1 : 0;
}

void gpu_gacco_init(void);
void gpu_gacco_exit(void);
void gpu_gacco_init_thread(void);
void gpu_gacco_exit_thread(void);
void gpu_gacco_begin(void);
void gpu_gacco_end(void);

void *gpu_gacco_malloc(size_t sz);
void  gpu_gacco_free(void *p);
void *gpu_gacco_calloc(size_t nmemb, size_t sz);
void *gpu_gacco_realloc(void *p, size_t sz);

uint8_t  gpu_gacco_read_i1(uint8_t *addr);
uint16_t gpu_gacco_read_i2(uint16_t *addr);
uint32_t gpu_gacco_read_i4(uint32_t *addr);
uint64_t gpu_gacco_read_i8(uint64_t *addr);
float    gpu_gacco_read_f4(float *addr);
double   gpu_gacco_read_f8(double *addr);
void    *gpu_gacco_read_ptr(void **addr);

void gpu_gacco_write_i1(uint8_t *addr, uint8_t val);
void gpu_gacco_write_i2(uint16_t *addr, uint16_t val);
void gpu_gacco_write_i4(uint32_t *addr, uint32_t val);
void gpu_gacco_write_i8(uint64_t *addr, int64_t val);
void gpu_gacco_write_f4(float *addr, float val);
void gpu_gacco_write_f8(double *addr, double val);
void gpu_gacco_write_ptr(void **addr, void *val);

int gpu_gacco_launch(int num_warps);
