/**
 * SPHT global variable definitions.
 * Include this file in exactly one translation unit (the runtime .cpp).
 */

#pragma once

#include "spht.hpp"

namespace spht
{

__thread Transaction *current_tx = nullptr;
__thread PCL *g_pcl = nullptr;
__thread sigjmp_buf *jmpbuf = nullptr;

std::atomic<uint64_t> g_num_threads{0};
std::atomic<uint64_t> *g_durable_seqs = nullptr;

} // namespace spht
