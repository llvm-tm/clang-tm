#ifndef QUEUE_RUNTIME_H
#define QUEUE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

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
void tm_enqueue(void (*fn)(void*), void* args);

// Block until all pending TXes enqueued by this thread complete.
// In inline mode: no-op (TX already completed synchronously).
// In queue mode: spin-waits on thread_local pending counter.
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
