#pragma once

// ── TM Debug Event Logging ──────────────────────────────────────
//
// Per-thread ring buffer records events without I/O (no serialization
// from printf mutexes or syscalls).  All macros expand to no-ops when
// NDEBUG is defined (release builds).
//
// ── Usage ─────────────────────────────────────────────────────────
//
//   Build with -UNDEBUG to enable.
//   
//   (1) Insert DBG_EVT at interesting points in the backend:
//         DBG_EVT(0, *addr);          // type=READ, val=counter
//         DBG_EVT(2, ts);             // type=COMMIT_OK, val=timestamp
//         DBG_EVT(6, re.old_version); // type=P3_FAIL, val=evidence
//
//   (2) Call tm_dbg_dump_all() at loss/crash detection to print
//       a cross-thread timeline sorted by wall clock:
//         T0 [98234781] READ val=18
//         T3 [98234783] WRITE_NEW val=19
//         T1 [98234784] P3_OK val=1
//
//   (3) To capture a specific counter without printf on every call,
//       store a counter pointer and check inside DBG_EVT:
//         static uint64_t *g_dbg_pcounter = ...;
//         DBG_EVT(0, g_dbg_pcounter ? *g_dbg_pcounter : 0);
//
// ── Finding event-ordering bugs with the timeline ─────────────────
//
//  Problem: Your STM loses increments in counter_mt (3-5%), no
//           printf allowed (masks the bug), pass-through backends
//           (WBCTL, TL2, NOrec) work fine.
//
//  Step 1: Insert DBG_EVT at read, write, commit, abort, and
//          validation-fail points in the suspect backend.
//
//  Step 2: Add a stop-on-bug check after commit (compares committed-op
//          count vs actual counter value via __atomic_load_n).
//
//  Step 3: Reproduce the loss.  The dump prints an interleaved
//          timeline of all events from all threads.  Look for:
//
//    • Counter values that go DOWN (e.g., READ val=7 then READ val=6)
//      → Something is restoring a stale value to memory.
//
//    • Old-value captures (type=EAGER) that are lower than the
//      committed value at the same moment → rollback wrote a stale
//      undo value, overwriting a committed increment.
//
//    • The timeline **without printf** exposes the exact interleaving
//      that lets a stale read slip through validation.
//
//  Step 4: When you find the pattern (e.g., rollback restores undo
//          value that is lower than current memory), fix the backend
//          to skip the restore when memory differs from the undo.
//
// ── Known fix (SwissTM, 2026-05-31) ──────────────────────────────
//
//  SwissTM's rollback() unconditionally restored the undo value for
//  every write-log entry.  When two TXs wrote the same counter and
//  one committed while the other aborted (Phase 3 mismatch), the
//  aborting TX's rollback restored its stale old_value to memory,
//  overwriting the committed TX's increment.  Fix:
//
//    // In rollback(), before restoring:
//    any_type_t cur = read_value_from_addr(entry.byte_addr, entry.type);
//    if (cur.u8 != entry.old_value.u8)  // memory changed — another TX committed
//        skip_restore();
//
//  This eliminated the 3-5% counter_mt loss and the bank benchmark
//  money-creation bug (+11 at 4 threads).
//
// ── Implementation details ───────────────────────────────────────
//
// Event types (user-defined, must be consistent across callers):
//   0 = read_value,   1 = write_new_val,  2 = commit_ok,
//   3 = abort,        4 = commit_ts,      5 = phase3_ok,
//   6 = phase3_fail,  7 = write_eager_old
//
// Threads register themselves on first DBG_EVT call (via thread_local
// function-local statics + a global registry with mutex).  Dumping
// logs requires the registry, so registration happens automatically.
// The sorted timeline uses steady_clock timestamps for cross-thread
// ordering (nanosecond precision on most platforms).
//
// ─────────────────────────────────────────────────────────────────

#include <cstdint>
#include <chrono>

#ifndef NDEBUG

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

constexpr int TM_DBG_LOG_SIZE = 16384;

struct TmDbgEvent {
	uint64_t wall;  // steady_clock timestamp
	uint16_t type;  // user-defined event type
	uint32_t seq;   // per-thread monotonic sequence
	uint64_t val;   // event value
};

static inline uint64_t tm_dbg_wall() {
	return std::chrono::steady_clock::now().time_since_epoch().count();
}

// Per-thread state (function-local to avoid ODR issues)
static inline TmDbgEvent *tm_dbg_log() {
	static thread_local TmDbgEvent buf[TM_DBG_LOG_SIZE];
	return buf;
}
static inline int &tm_dbg_idx() {
	static thread_local int idx = 0;
	return idx;
}
static inline uint64_t &tm_dbg_seq() {
	static thread_local uint64_t seq = 0;
	return seq;
}

// Global registry of per-thread logs + indices
static std::mutex &tm_dbg_mutex() {
	static std::mutex m;
	return m;
}
static std::vector<TmDbgEvent*> &tm_dbg_logs() {
	static std::vector<TmDbgEvent*> v;
	return v;
}
static std::vector<int*> &tm_dbg_idxs() {
	static std::vector<int*> v;
	return v;
}
static int &tm_dbg_registered() {
	static thread_local int reg = 0;
	return reg;
}

// Auto-register this thread's log buffer on first DBG_EVT call.
static inline void tm_dbg_ensure_registered() {
	if (tm_dbg_registered()) return;
	std::lock_guard<std::mutex> lock(tm_dbg_mutex());
	tm_dbg_logs().push_back(tm_dbg_log());
	tm_dbg_idxs().push_back(&tm_dbg_idx());
	tm_dbg_registered() = 1;
}

#define DBG_EVT(t, v) do { \
	tm_dbg_ensure_registered(); \
	TmDbgEvent *__log = tm_dbg_log(); \
	int &__idx = tm_dbg_idx(); \
	if (__idx < TM_DBG_LOG_SIZE) { \
		__log[__idx] = {tm_dbg_wall(), (uint16_t)(t), \
		                (uint32_t)tm_dbg_seq()++, (uint64_t)(v)}; \
		__idx++; \
	} \
} while(0)

// Dump all registered thread logs to stderr.
static inline void tm_dbg_dump_all() {
	std::lock_guard<std::mutex> lock(tm_dbg_mutex());
	int total_events = 0;
	for (size_t t = 0; t < tm_dbg_logs().size(); t++) {
		int end = *tm_dbg_idxs()[t];
		total_events += end;
	}
	if (total_events == 0) return;

	// Collect all events, sort by wall clock
	std::vector<std::pair<uint64_t, std::pair<int, int>>> sorted;
	sorted.reserve(total_events);
	for (size_t t = 0; t < tm_dbg_logs().size(); t++) {
		TmDbgEvent *log = tm_dbg_logs()[t];
		int end = *tm_dbg_idxs()[t];
		for (int i = 0; i < end && i < TM_DBG_LOG_SIZE; i++)
			sorted.push_back({log[i].wall, {t, i}});
	}
	std::sort(sorted.begin(), sorted.end());

	fprintf(stderr, "\n=== Interleaved timeline (%zu events) ===\n", sorted.size());
	for (auto &p : sorted) {
		int t = p.second.first;
		int i = p.second.second;
		auto &e = tm_dbg_logs()[t][i];
		const char *type_str = "?";
		switch (e.type) {
			case 0: type_str = "READ"; break;
			case 1: type_str = "WRITE_NEW"; break;
			case 2: type_str = "COMMIT_OK"; break;
			case 3: type_str = "ABORT"; break;
			case 5: type_str = "P3_OK"; break;
			case 6: type_str = "P3_FAIL"; break;
			case 7: type_str = "EAGER"; break;
		}
		fprintf(stderr, "T%d [%llu] %s val=%lu\n",
		        t, (unsigned long long)e.wall, type_str,
		        (unsigned long)e.val);
	}
	fflush(stderr);
}

#else
// Release build: all macros/functions are no-ops.
#define DBG_EVT(t, v) ((void)0)
static inline void tm_dbg_dump_all() {}
#endif // NDEBUG
