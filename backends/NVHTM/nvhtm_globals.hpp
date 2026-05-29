/**
 * NV-HTM global variable definitions.
 * Include this file in exactly one translation unit (the runtime .cpp).
 */

#pragma once

#include "nvhtm.hpp"

namespace nvhtm
{

__thread Transaction *current_tx = nullptr;
__thread sigjmp_buf *jmpbuf = nullptr;

} // namespace nvhtm
