/** -------------------------------------------------------
  * Include this file in a source file to define the global
  * variables.
  * ----------------------------------------------------- */
#pragma once

#include "NOrec_BF.hpp"

namespace norecbf
{
std::atomic<norecbf::word_t> global_lock{0};
std::atomic<norecbf::word_t> thr_counter{1};
std::atomic<uint64_t> g_tm_abort_count{0};
__thread Transaction *current_tx = nullptr;
__thread sigjmp_buf *jmpbuf;

// ── NOrec-BF committed-writes Bloom filter ────────────────────────
// g_gc: global filter accumulating addresses written by committed
//       transactions.  Writers insert under the global lock (commit).
// g_gc_gen: generation counter.  When the filter saturates, a writer
//       bumps the generation and clears the filter.  A transaction
//       records the generation at begin(); if the filter was rebuilt
//       while it was in flight, its read-set fast-path validation is
//       disabled (falls back to exact value re-reads).
std::atomic<uint64_t> g_gc_gen{0};
stm::BloomFilter<kBloomWords> g_gc;
} // namespace norecbf
