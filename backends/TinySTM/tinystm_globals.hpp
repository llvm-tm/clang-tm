/** -------------------------------------------------------
  * Include this file in a source file to define the global
  * variables.
  * ----------------------------------------------------- */
#pragma once

#if defined(DESIGN_WBETL)
#include "tinystm_wbetl.hpp"
namespace tinystm
{
LockTable<Lock_wbetl> g_locks_wbetl;
__thread Transaction<ReadLogEntry_wbetl, WriteLogEntry_wbetl> *current_tx_wbetl = nullptr;
thread_local bool rng_initialized = false;
thread_local std::mt19937 rng;
std::atomic<tinystm::word_t> reset_locks_thr{0};
std::atomic<tinystm::word_t> g_clock{1};
std::atomic<tinystm::word_t> thr_counter{1};
std::atomic<uint64_t> g_tm_abort_count{0};
__thread sigjmp_buf *jmpbuf;
} // namespace tinystm
#elif defined(DESIGN_WBCTL)
#include "tinystm_wbctl.hpp"
namespace tinystm
{
LockTable<Lock_wbctl> g_locks_wbctl;
__thread Transaction<ReadLogEntry_wbctl, WriteLogEntry_wbctl> *current_tx_wbctl = nullptr;
thread_local bool rng_initialized = false;
thread_local std::mt19937 rng;
std::atomic<tinystm::word_t> reset_locks_thr{0};
std::atomic<tinystm::word_t> g_clock{1};
std::atomic<tinystm::word_t> thr_counter{1};
std::atomic<uint64_t> g_tm_abort_count{0};
__thread sigjmp_buf *jmpbuf;
} // namespace tinystm
#elif defined(DESIGN_WT)
#include "tinystm_wt.hpp"
// wt.hpp defines its own globals (g_locks, current_tx, rng, g_clock, etc.)
namespace tinystm {
std::atomic<uint64_t> g_tm_abort_count{0};
__thread sigjmp_buf *jmpbuf;
}
#else
#error "Define one of the following: DESIGN_WBETL, DESIGN_WBCTL, DESIGN_WT"
#endif
