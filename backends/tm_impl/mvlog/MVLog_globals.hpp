/** -------------------------------------------------------
  * Include this file in a source file to define the global
  * variables.
  * ----------------------------------------------------- */
#pragma once

#include "MVLog.hpp"

namespace mvlog
{
std::atomic<uint64_t> g_next{1};
std::atomic<uint64_t> g_wm{0};
std::atomic<uint32_t> g_commit_lock{0};
LogEntry g_log[kLogSlots];
IndexBucket g_index[kIndexSlots];
stm::BloomFilter<64> g_dirty;
std::atomic<uint64_t> g_tm_abort_count{0};
__thread Transaction *current_tx = nullptr;
__thread sigjmp_buf *jmpbuf;
} // namespace mvlog
