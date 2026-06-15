#ifndef QUEUE_RUNTIME_H
#define QUEUE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include <atomic>

// Global (non-TLS) flag: 1 when queue runtime is active (tm_queue_init called).
// Unlike the TLS g_tm_queue_active (only set for the enqueuing thread), this is
// visible to all threads.  Backends (e.g. LeftRight) use it to skip barrier
// synchronization that would deadlock when only worker threads run TM.
extern std::atomic<int> g_tm_queue_global;

#ifdef __cplusplus
extern "C" {
#endif

// Enqueue a TX for execution.
// fn: dispatch function that calls the instrumented _tm_clone.
// args: heap-allocated packed arguments struct (freed by dispatch after use).
//
// In inline mode: calls fn(args) immediately.
// In queue mode: increments thread_local pending counter, enqueues work,
//                worker decrements the CALLER's counter on completion.
//
// DATA variable (function pointer) — the LLVM pass declares this as
// external global ptr and emits indirect calls through it.
extern void (*tm_enqueue)(void (*fn)(void*), void* args);

// Extended enqueue with queue routing and transaction ID.
// queue_id: target queue index (0..num_queues-1), or -1 for default round-robin.
// Returns: a globally unique transaction ID, or 0 if executed inline.
// The caller can later pass this ID to tm_wait_tx() to block until that
// specific transaction completes (not necessarily the last one enqueued).
uint64_t tm_enqueue_ex(void (*fn)(void*), void* args, int queue_id);

// Block until the transaction identified by tx_id completes.
// Safe to call from any thread, but only the enqueuing thread's
// completion tracking is consulted.
void tm_wait_tx(uint64_t tx_id);

// Return the last transaction ID enqueued by this thread, or 0 if none.
uint64_t tm_last_tx_id(void);

// Block until all pending TXes enqueued by this thread complete.
// In inline mode: no-op (TX already completed synchronously).
// In queue mode: spin-waits on thread_local pending counter.
//
// DATA variable (function pointer) — the LLVM pass declares this as
// external global ptr and emits indirect calls through it.
extern void (*tm_wait_prev_tx)(void);

// Initialize thread pool.  Called once at program startup.
// Workers read from THREADS env var, fallback to default_workers.
// Queues use same count, or default_queues.
void tm_queue_init(int default_workers, int default_queues);

// Shut down thread pool.  Called at program exit.
void tm_queue_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // QUEUE_RUNTIME_H
