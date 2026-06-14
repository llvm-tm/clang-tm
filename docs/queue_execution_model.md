# Queue-Based TM Execution Model

## Motivation

The current TM execution model is **inline-only**: a transaction runs on the thread that calls `transaction()`. This couples TM concurrency with application thread structure — every thread that runs TXs must spawn and manage its own worker, and TX throughput is limited by the calling thread's availability.

A queue-based model decouples these concerns. The caller enqueues a TX to a thread pool. This enables:

- **Elastic parallelism**: application threads can be independent of TM worker threads
- **Load-balanced TM**: the pool can absorb bursts and smooth TX throughput
- **Flexible work distribution**: multiple queues with hash-based or priority routing

The model provides two transaction types:
- **`transaction`** (default): synchronous from the app's perspective — the plugin enqueues the TX and then **waits** for it to complete before returning to the app. The queue provides elastic worker resources under the hood, but the calling thread still blocks until the TX finishes.
- **`async_transaction`** (optional): the plugin enqueues the TX and returns immediately. The app can continue working and later call `tm_wait_prev_tx()` to block until the TX completes. In inline mode, `async_transaction` behaves identically to `transaction` and `tm_wait_prev_tx()` is a no-op.

## Architecture Layers

```
Application Code (C++ explicit API or LLVM plugin)
    │
    ├── Layer 3: TM<T>::transaction() / TM<T>::async()
    │            (user-facing wrapper, retry loop, nesting)
    │
    ├── Layer 2: TxExecutor (runtime execution model)
    │            ╔══════════════════════════════╗
    │            ║ InlineExecutor               ║
    │            ║   enqueue → invoke inline    ║
    │            ╚══════════════════════════════╝
    │            ╔══════════════════════════════╗
    │            ║ QueueExecutor                ║
    │            ║   enqueue → push to work Q   ║
    │            ║   worker pool picks up, runs ║
    │            ╚══════════════════════════════╝
    │
    └── Layer 1: TM Backend (WBCTL, NOrec, TL2, ...)
                 tm_begin / tm_commit / tm_read / tm_write
```

Each layer knows nothing about the layer above it. The backend provides `tm_begin`/`tm_commit`/`tm_read`/`tm_write` only — it doesn't know about executors or retry loops. The executor provides `enqueue(task)` — it doesn't know about TM backends or retry loops.

## TxExecutor Interface

```cpp
// backends/tm_executor.hpp
class TxExecutor {
public:
    virtual ~TxExecutor() = default;

    // Submit a task for execution. Thread-safe.
    virtual void enqueue(std::function<void()> task) = 0;

    // Block until all submitted tasks complete.
    virtual void wait_all() = 0;

    // Shut down the executor; no new tasks accepted.
    virtual void shutdown() = 0;
};
```

## InlineExecutor (Existing Behavior)

```cpp
class InlineExecutor : public TxExecutor {
public:
    void enqueue(std::function<void()> task) override {
        task();  // execute immediately, blocking
    }
    void wait_all() override {}
    void shutdown() override {}
};
```

This is a thin wrapper around the existing `transaction()` call. The TM retry loop (sigsetjmp/siglongjmp) still lives inside the task — the executor just chooses when to invoke it.

## QueueExecutor (New)

```cpp
class QueueExecutor : public TxExecutor {
public:
    QueueExecutor(int num_workers, int num_queues);
    ~QueueExecutor();

    // Parameterizable routing policy per-task:
    //   HashPolicy  → queue = hash(task) % num_queues
    //   RoundRobin  → queue = next_atomic_counter % num_queues
    //   Direct      → user specifies queue index
    template <typename Routing = HashPolicy>
    void enqueue(std::function<void()> task, int queue_hint = -1);

    void wait_all() override;
    void shutdown() override;
};
```

### Worker Loop

```
for each worker thread:
    while (active):
        task = dequeue()       // blocking pop from assigned queue
        task()
```

Dequeue is blocking (condition variable wait) when the queue is empty, and a single dequeue steals from sibling queues on timeout for load balance.

### Queue Selection

- **HashPolicy**: `queue = hash(&task) % num_queues` — deterministic, same TX always goes to same queue (better cache locality if data is partitioned)
- **RoundRobin**: `queue = next.fetch_add(1) % num_queues` — uniform distribution, simpler
- **Direct**: caller specifies queue index — for explicit data partitioning

### Number of Workers vs Queues

| Pattern | Workers (N) | Queues (M) | Behavior |
|---------|-------------|------------|----------|
| Classic pool | ≥1 | 1 | All workers compete on one queue |
| Per-worker | = N | = N | One queue per worker, work stealing |
| Partitioned | ≥1 | > N | Data is hashed to M buckets, workers handle buckets |

For STM, **M = N** (per-worker) is the natural default: each worker has its own queue, and hash-based routing sends the same task to the same worker. Load imbalance is handled by work stealing (periodic idle attempts).

## Sync vs Async Transaction Semantics

The key design rule: **`transaction` is always synchronous from the app's perspective**, regardless of execution model.

| Execution mode | `transaction` | `async_transaction` | `tm_wait_prev_tx()` |
|----------------|---------------|---------------------|---------------------|
| **Inline** | execute inline, blocking | execute inline, blocking _(same as transaction)_ | no-op (TX already done) |
| **Queue** | enqueue → **wait** (plugin-inserted) | enqueue → return immediately | blocks until prev async TX done |

The app always sees the same semantics for `transaction`: the TX completes before the next line of code runs. Only `async_transaction` gives asynchronous behavior, and only in queue mode.

## Integration with Explicit C++ API

The explicit C++ API follows the same pattern using thread-local state:

```cpp
// Thread-local future handle (in runtime):
thread_local struct {
    bool               queue_active   = false;  // set during init
    std::promise<void> pending_promise;          // fulfilled by worker
    std::future<void>  pending_future;           // waited on by caller
    bool               has_pending    = false;
} g_tx_state;

// tm_enqueue(body):
//   - In inline mode: execute body inline (no future needed)
//   - In queue mode: create promise, store in g_tx_state,
//     push body to worker queue

// tm_wait_prev_tx():
//   - In inline mode: no-op
//   - In queue mode: g_tx_state.pending_future.wait()
//     (blocks until worker signals the promise)
```

### TM<T> wrappers

```cpp
// Synchronous (always blocks until TX completes):
template <typename F>
static void transaction(F&& body) {
    // InlineExecutor (existing, unchanged):
    InlineExecutor exec;
    exec.submit<Backend>(std::forward<F>(body));
}

// Async (queue mode only; inline mode = synchronous):
// The plugin inserts tm_wait_prev_tx() for regular transaction.
// For async_transaction, the app calls tm_wait_prev_tx() manually.
```

For the explicit API, the `transaction()` method uses `InlineExecutor` (unchanged behavior). Users who want queue mode with the explicit API create a `QueueExecutor` directly and call `exec.submit<Backend>(body)` which returns a future, then call `tm_wait_prev_tx()` or `future.get()` to synchronize.

## Sync vs Async Transaction Semantics (Detail)

### Default `transaction` annotation (queue mode)

```
App Thread:                          Worker Thread:
  tm_enqueue(dispatch, args) ──────> dequeue task
  tm_wait_prev_tx() (block until ──> execute dispatch()
    g_tx_state.pending_future done)    tm_begin()
  continue ...                         tm_body(args)
                                       tm_commit()
                                       signal promise ───> g_tx_state.pending_future resolves
```

The plugin inserts `tm_wait_prev_tx()` after every `tm_enqueue` call for `transaction`-annotated functions. The app thread blocks until the worker finishes.

### `async_transaction` annotation (queue mode)

```
App Thread:                          Worker Thread:
  tm_enqueue(dispatch, args) ──────> dequeue task (sometime later)
  continue ... ──────────────────>    execute dispatch()
    (app does other work)               tm_begin()
  tm_wait_prev_tx() (block until ──>   tm_body(args)
    g_tx_state.pending_future done)     tm_commit()
  continue ...                          signal promise ───> resolves
```

The app calls `tm_wait_prev_tx()` manually when it needs the TX to have completed.

### Inline mode (both annotations)

```
App Thread:
  tm_enqueue(dispatch, args) ──> dispatch() executes inline
                                    tm_begin() / tm_body() / tm_commit()
  tm_wait_prev_tx() ───────────────> no-op (g_tx_state.queue_active == false)
  continue ...
```

In inline mode, `async_transaction` behaves exactly like `transaction`. `tm_wait_prev_tx()` is a no-op because the inline `tm_enqueue` already completed the TX synchronously.

## Runtime Thread-Local State

The runtime maintains per-thread state to bridge the enqueuing thread and the worker thread:

```c
// backends/tm_impl/queue/queue_runtime.h
typedef struct tm_future_state {
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    int              done;      // set to 1 by worker after TX completes
} tm_future_state_t;

// Per-thread state:
extern _Thread_local int               g_tm_queue_active;    // 1 = queue mode
extern _Thread_local tm_future_state_t *g_tm_pending;        // current TX's future
```

### `tm_enqueue` (C runtime)

```c
void tm_enqueue(void (*fn)(void*), void* args) {
    if (!g_tm_queue_active) {
        // Inline mode: execute synchronously
        fn(args);
        return;
    }

    // Queue mode: create a future for this TX
    tm_future_state_t* state = malloc(sizeof(tm_future_state_t));
    state->done = 0;
    pthread_mutex_init(&state->mutex, NULL);
    pthread_cond_init(&state->cond, NULL);

    // Replace previous pending (leak if app never waited on it)
    g_tm_pending = state;

    // Push to thread pool
    pool_enqueue([fn, args, state]() {
        fn(args);   // runs dispatch() → tm_begin/tm_body/tm_commit
        pthread_mutex_lock(&state->mutex);
        state->done = 1;
        pthread_cond_signal(&state->cond);
        pthread_mutex_unlock(&state->mutex);
        free(args);
    });
}
```

### `tm_wait_prev_tx` (C runtime)

```c
void tm_wait_prev_tx(void) {
    if (!g_tm_queue_active || !g_tm_pending) {
        return;   // inline mode or no pending TX
    }

    tm_future_state_t* state = g_tm_pending;
    g_tm_pending = NULL;   // consume

    pthread_mutex_lock(&state->mutex);
    while (!state->done) {
        pthread_cond_wait(&state->cond, &state->mutex);
    }
    pthread_mutex_unlock(&state->mutex);

    pthread_mutex_destroy(&state->mutex);
    pthread_cond_destroy(&state->cond);
    free(state);
}
```

`g_tm_pending` is thread-local, so only the thread that called `tm_enqueue` can call `tm_wait_prev_tx()`. Worker threads never call these functions.

## Integration with LLVM Plugin

The plugin currently has three pipelines:
1. `tm-instrument` — CloneOnly + instrument, NoInline clones, separate `-O3` pass  
2. `tm-instrument-inline` — AlwaysInline, inline after instrument
3. `tm-instrument-then-inline` — Instrument + AlwaysInline

A new pipeline `tm-instrument-queue` would:

### 1. Annotations

The plugin recognizes two TX annotations:
- `__attribute__((annotate("transaction")))` — synchronous in all modes
- `__attribute__((annotate("async_transaction")))` — async in queue mode, sync in inline mode

Both annotations are processed identically for cloning and instrumentation. The difference is only in the generated call site code.

### 2. Clone + dispatch wrapper (same for both annotations)

Same as existing: clone `foo` → `foo_tm_clone`. Then generate a dispatch wrapper:

```c
// Generated by plugin for every _tm_clone:
void foo_tm_clone_dispatch(void* raw_args) {
    foo_tm_clone_args* args = (foo_tm_clone_args*)raw_args;
    volatile int done = 0;
    while (!done) {
        if (sigsetjmp(tm_jmpbuf, 0) == 0) {}
        tm_begin();
        foo_tm_clone(args->a1, args->a2, ...);
        done = tm_commit();
    }
    free(raw_args);
}
```

### 3. Call site replacement

**For `transaction` annotation:**

```llvm
; Original: call void @foo(args...)

; Replaced with:
%args = call i8* @malloc(i64 sizeof_args)
store args... into %args
call void @tm_enqueue(void* @foo_tm_clone_dispatch, i8* %args)
call void @tm_wait_prev_tx()    ; ← plugin inserts this wait
```

The plugin inserts `tm_wait_prev_tx()` right after `tm_enqueue`. This makes the TX synchronous from the app's perspective even though it runs on a worker thread.

**For `async_transaction` annotation:**

```llvm
; Original: call void @foo(args...)

; Replaced with:
%args = call i8* @malloc(i64 sizeof_args)
store args... into %args
call void @tm_enqueue(void* @foo_tm_clone_dispatch, i8* %args)
; NO tm_wait_prev_tx() — app must call it later
```

The plugin does NOT insert `tm_wait_prev_tx()` after the enqueue. The app is responsible for calling `tm_wait_prev_tx()` when it needs the TX to have completed.

### 4. `tm_wait_prev_tx()` in user code

User code may call `tm_wait_prev_tx()` explicitly:

```c
__attribute__((annotate("async_transaction")))
void do_work(int* data) {
    *data += 1;
}

void app() {
    do_work(&counter);   // enqueues, returns immediately
    // ... do other work ...
    tm_wait_prev_tx();   // wait for do_work to complete
}
```

The plugin leaves `tm_wait_prev_tx()` calls unchanged (they resolve to the runtime function). In queue mode, the runtime blocks until the previous async TX finishes. In inline mode, it's a no-op.

### 5. Inline pipeline behavior (unchanged)

For inline pipelines, both `transaction` and `async_transaction` produce the same code:
```llvm
call void @tm_begin()
call void @foo_tm_clone(args...)
call i1 @tm_commit()
```

The runtime's `g_tm_queue_active` is `false` in inline mode, so `tm_enqueue` (if somehow called) would execute inline and `tm_wait_prev_tx()` would be a no-op.

### Annotation validation

The plugin's `checkAnnotationConsistency` must recognize both `"transaction"` and `"async_transaction"`. A function cannot have both annotations.

For queue pipelines, any `async_transaction` without a corresponding `tm_wait_prev_tx()` before the function returns is a potential leak of the thread-local future state — this could be a warning.

### Plugin flag selection

```
opt -passes="tm-instrument-queue" \
    -tm-num-workers=4 \
    -tm-num-queues=4
```

## Avoiding Code Duplication

The key principle: **the backend never changes**. It provides `tm_begin`/`tm_commit`/`tm_read`/`tm_write` regardless of execution model.

| Concern | Where | Shared? |
|---------|-------|---------|
| `tm_begin/commit/read/write` | Backend `.hpp` | Yes, all executors |
| Retry loop (sigsetjmp) | Dispatch wrapper (plugin-generated) | No, new code per TX fn |
| Thread pool | `QueueExecutor` class | No, new code |
| Plugin clone generation | `TMInstrumentPass` | Yes, same cloning code |
| Plugin dispatch wrapper | New pass in `tm-instrument-queue` pipeline | No, new code |
| `tm_enqueue` / `tm_wait_prev_tx` | `backends/tm_impl/queue/queue_runtime.cpp` | No, new code |
| Thread-local future state | `backends/tm_impl/queue/queue_runtime.h` | No, new code |
| `async_transaction` annotation support | Plugin annotation checker | new |
| `tm_wait_prev_tx()` call insertion | Plugin (for `transaction` in queue mode) | new |

**What is reused:**
- All existing backend code (WBCTL, NOrec, TL2, SwissTM, etc.)
- Plugin clone generation (`computeClonableFunctions`, `redirectCallsToClones`, `instrumentLoadStoresInFunction`)
- Plugin annotation infrastructure (`checkAnnotationConsistency`)
- `TM<T>` template (inline path unchanged)
- `tm_alloc_overrides.hpp`, `tm_debug.hpp`, `tm_platform.hpp`

**What is new:**
- `backends/tm_impl/queue/queue_runtime.h` — `tm_future_state_t`, `tm_enqueue`, `tm_wait_prev_tx` declarations
- `backends/tm_impl/queue/queue_runtime.cpp` — thread pool + `tm_enqueue`/`tm_wait_prev_tx` impl
- Plugin pipeline: `tm-instrument-queue`
- Plugin: `async_transaction` annotation recognition + `tm_wait_prev_tx` call insertion
- Dispatch wrapper generation in plugin

## Implementation Plan

### Step 1: Runtime thread-local state + C API
- `backends/tm_impl/queue/queue_runtime.h` — `tm_future_state_t`, `g_tm_queue_active`, `g_tm_pending`
- `backends/tm_impl/queue/queue_runtime.cpp` — `tm_enqueue`, `tm_wait_prev_tx`, thread pool `QueueExecutor`
- No-op path when `g_tm_queue_active == false`

### Step 2: Plugin annotation support
- Add `"async_transaction"` to recognized annotations in `checkAnnotationConsistency`
- Extend the annotation-to-pipeline mapping to handle both `transaction` and `async_transaction`

### Step 3: Plugin queue pipeline
- New pipeline `tm-instrument-queue` in `TMInstrumentPass.cpp`
- Clone + dispatch wrapper generation (same cloning code, new IR generation for wrapper)
- Call site replacement: `call @foo(args...)` → `tm_enqueue` + (for `transaction`) `tm_wait_prev_tx`

### Step 4: Runtime thread pool
- `QueueExecutor` class with N workers × M queues
- `pool_init(num_workers, num_queues)` called at startup
- Worker loop: blocking dequeue → execute → signal promise

### Step 5: Tests
- `test_queue_sync`: single `transaction` in queue mode — verify it blocks until completion
- `test_queue_async`: single `async_transaction` + `tm_wait_prev_tx` — verify ordering
- `test_queue_multi`: multiple TXs on queue, all complete correctly
- `test_queue_inline_mode`: `async_transaction` in inline mode behaves like `transaction`

### Step 6: Benchmarks
- Compare inline vs queue throughput on bank/eigenbench/labyrinth at 1t/2t/4t
- Verify correctness (money conservation, counter accuracy)

## Rust Equivalent

The Rust API follows the same pattern with thread-local `Cell` or `RefCell` for pending future state:

```rust
// thread_local! for pending TX state
thread_local! {
    static TX_STATE: RefCell<TxState> = RefCell::new(TxState {
        queue_active: false,
        pending: None,
    });
}

// tm_enqueue:
//   - Inline mode: execute closure inline
//   - Queue mode: push to thread pool, store JoinHandle in TX_STATE

// tm_wait_prev_tx():
//   - Inline mode: no-op
//   - Queue mode: block on the stored JoinHandle

// Generated by the Rust equivalent of the plugin:
fn foo_tm_clone_dispatch(args: FooArgs) {
    let mut done = false;
    while !done {
        tm_begin();
        foo_tm_clone(&args);
        done = tm_commit();
    }
}

let exec = Arc::new(QueueExecutor::new(4, 4));
tm_enqueue(&exec, foo_tm_clone_dispatch, args);
// For transaction: tm_wait_prev_tx() follows
// For async_transaction: user calls tm_wait_prev_tx() later
```
