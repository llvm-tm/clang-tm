#pragma once
// GPU detection and shared state for all GPU TM backends.
// Compiled once to avoid duplicate gpu_available symbol.

#include "tm_gpu_platform.hpp"

// Returns 1 if GPU is available, 0 otherwise.
int gpu_check_and_set(int fallback_value);

// Shared flag: 1 = GPU available, 0 = CPU fallback
// Defined in tm_gpu_detect.cpp, declared extern here.
extern int g_gpu_available;
extern uint32_t g_gpu_lock_table_size;
