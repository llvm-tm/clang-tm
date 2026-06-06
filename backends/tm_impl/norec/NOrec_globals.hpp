/** -------------------------------------------------------
  * Include this file in a source file to define the global
  * variables.
  * ----------------------------------------------------- */
#pragma once

#include "NOrec.hpp"

namespace norec
{
std::atomic<norec::word_t> global_lock{0};
std::atomic<norec::word_t> thr_counter{1};
std::atomic<uint64_t> g_tm_abort_count{0};
__thread Transaction *current_tx = nullptr;
__thread sigjmp_buf *jmpbuf;
} // namespace norec
