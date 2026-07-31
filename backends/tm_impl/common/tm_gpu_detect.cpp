#include "tm_gpu_detect.hpp"

int g_gpu_available = 0;
uint32_t g_gpu_lock_table_size = 1 << 20;

int gpu_check_and_set(int fallback_value) {
    if (g_gpu_available) return 1;
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err == cudaSuccess && count > 0) {
        g_gpu_available = 1;
    } else {
        g_gpu_available = fallback_value;
    }
    return g_gpu_available;
}
