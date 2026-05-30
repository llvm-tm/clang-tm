#pragma once

#include <atomic>
#include <cstdint>

// ── Global Spin Token ─────────────────────────────────────────
//
// Tie-breaker for deadlocks and livelocks in TM backends.
//
// At most one thread holds the token at any time.  When a thread
// holds the token and encounters a contended lock, it may SPIN
// instead of aborting.  Non-holders must abort.
//
// The holder releases the token on successful commit (or abort).
// Token acquisition is lazy — only attempted on contention, and
// only after a configurable abort-count threshold (e.g., 3 for
// encounter-time-locking STMs, 5 for commit-time-locking STMs).
//
// This breaks the ABORT→RETRY→ABORT storm: the token holder's TX
// makes progress while everyone else backs off.  The threshold
// prevents trivial contention from acquiring the token, and the
// `is_free()` pre-check avoids unnecessary CAS failures.
//
// Since thread exit is orthogonal (no explicit tm_thread_exit
// callbacks in the current platform), the token uses CAS(-1→tx_id)
// so that any stray thread exit naturally leaves g_tx_token=-1.
// On abort, tm_token_release_if_held() cleans up.
//
// Architecture-appropriate spin-loop hint
#if defined(__x86_64__) || defined(__i386__)
#define TINY_STM_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define TINY_STM_PAUSE() __builtin_arm_yield()
#else
#define TINY_STM_PAUSE() ((void)0)
#endif

namespace stm
{

// -1 means free; otherwise holds the thread ID that acquired it
inline std::atomic<int64_t> g_tx_token{-1};

/// Returns true if the token is currently free (no thread holds it).
/// Use as a cheap optimistic check before attempting acquire.
inline bool tm_token_is_free()
{
	return g_tx_token.load(std::memory_order_acquire) == -1;
}

/// Try to acquire the token.  Returns true if this thread now holds it.
inline bool tm_token_try_acquire(int64_t tx_id)
{
	int64_t expected = -1;
	return g_tx_token.compare_exchange_strong(expected,
	                                          tx_id,
	                                          std::memory_order_acquire,
	                                          std::memory_order_relaxed);
}

/// Soft-spin check for use at contention points.
/// Returns true if the caller should spin on the contended lock
/// (token was acquired).  Returns false on failure — the caller
/// MUST abort the transaction (or use its backend-specific fallback).
/// `threshold` is the minimum abort count before attempting the token
/// (e.g. 5 for commit-time-locking STMs, 3 for encounter-time).
inline bool tm_token_soft_spin(int abort_count, int64_t tx_id, int threshold)
{
	if (abort_count < threshold)
		return false;
	if (!tm_token_is_free())
		return false;
	return tm_token_try_acquire(tx_id);
}

/// Release the token unconditionally (called after successful commit).
inline void tm_token_release() { g_tx_token.store(-1, std::memory_order_release); }

/// Release only if this thread currently holds the token.
/// Useful for thread-exit cleanup.
inline void tm_token_release_if_held(int64_t tx_id)
{
	int64_t expected = tx_id;
	g_tx_token.compare_exchange_strong(expected,
	                                   -1,
	                                   std::memory_order_release,
	                                   std::memory_order_relaxed);
}

// Legacy — kept for compatibility with callers that set num_threads.
// The CAS token does not use a modulo, so this is a no-op.
inline void tm_token_set_num_threads(uint32_t) {}

} // namespace stm
