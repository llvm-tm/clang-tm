#ifndef QUEUE_RUNTIME_H
#define QUEUE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TLS variables defined in queue_runtime.cpp, accessed by the LLVM plugin
// via M.getGlobalVariable().  Not declared here — the plugin finds them by name.

// Enqueue a transaction for execution.
// fn: dispatch function that calls the instrumented _tm_clone.
// args: heap-allocated packed arguments struct (freed by dispatch after use).
//
// In inline mode: calls fn(args) immediately (no-op future).
// In queue mode: creates a future, stores in thread_local, pushes to pool.
void tm_enqueue(void (*fn)(void*), void* args);

// Block until the previous async TX on this thread completes.
// In inline mode: no-op (TX already completed synchronously).
// In queue mode: blocks on the thread_local future.
void tm_wait_prev_tx(void);

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
