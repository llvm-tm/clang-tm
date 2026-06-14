# TM Simulator — Automated Runtime Simulation

## 1. Principle

**No hand-porting of backend algorithms.** The simulator links the actual Rust backend crates
(`runtime-norec`, `runtime-tl2`, `runtime-tinystm`, etc.) as library dependencies and calls
their real `tm_begin()`/`tm_read()`/`tm_write()`/`tm_commit()` functions directly.

The only mechanical change to backends: a `simulation` Cargo feature flag that replaces
`thread_local!` storage with an explicit `HashMap<ThreadId, State>` so the scheduler can
multiplex simulated threads.

---

## 2. Architecture

```
┌───────────────────┐   trace.jsonl    ┌───────────────────────────────────────────────┐
│ Trace Sources     │ ──────────────→  │ tm-sim                                       │
│                   │                  │                                               │
│  • tm-gen         │                  │  ┌──────────────┐  ┌───────────────────────┐  │
│  • LLVM plugin    │                  │  │ Scheduler    │  │ Actual Backend Crate  │  │
│    --emit-tm-trace│                  │  │ (determin-   │──│ (linked as dep)       │  │
│  • C++ hooks      │                  │  │  istic inter-│  │                       │  │
│    TM_TRACE_PATH  │                  │  │  leaving)    │  │ runtime-norec/src     │  │
│                   │                  │  └──────┬───────┘  │ runtime-tl2/src       │  │
└───────────────────┘                  │         │          │ runtime-tinystm/src   │  │
                                       │         │          │ ...                   │  │
┌───────────────────┐  --backend      │  ┌──────▼────────┐ └───────────────────────┘  │
│ CLI selects       │ ──────────────→  │  │ Thread Pool   │                           │
│ crate to link     │                  │  │ (real threads,│  ┌─────────────────────┐  │
└───────────────────┘                  │  │  1-at-a-time  │  │ Verifier            │  │
                                       │  │  scheduling) │  │ • Shadow memory     │  │
                                       │  └──────────────┘  │ • Deadlock detect   │  │
                                       │                     │ • Opacity check     │  │
                                       │                     │ • Money conservation│  │
                                       │                     └─────────────────────┘  │
                                       └──────────────────────────────────────────────┘
```

### Flow

1. Simulator reads JSONL trace events.
2. For each event, the scheduler selects which simulated thread runs next.
3. The thread's real OS thread processes one event by calling the backend's actual
   `tm_begin()`, `tm_read()`, etc.
4. The verifier intercepts and checks the operation.
5. The thread signals done; the scheduler picks the next thread.
6. Only one thread executes at any instant — deterministic interleaving.

---

## 3. The Simulation Feature Flag (`simulation`)

### Problem

Every Rust backend uses `thread_local!` for per-thread TxState:

```rust
thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}
```

A DES scheduler multiplexes many simulated threads onto a small number of real OS threads.
`thread_local!` binds state to OS threads, making it impossible to swap simulated threads
on the same OS thread.

### Solution

Add a `simulation` feature to `runtime-core` and each backend that switches storage:

```rust
// Without simulation (normal, production):
thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}
fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    TX.with(|tx| f(tx.borrow_mut().as_mut().expect("no active TX")))
}

// With simulation (deterministic replay):
use std::collections::HashMap;
use std::sync::Mutex;

static SIM_TX: Mutex<HashMap<ThreadId, Option<Box<TxState>>>> = Mutex::new(HashMap::new());

thread_local! {
    static SIM_THREAD_ID: Cell<Option<ThreadId>> = const { Cell::new(None) };
}

fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    let tid = SIM_THREAD_ID.with(|id| id.get().expect("sim_thread_id not set"));
    let mut map = SIM_TX.lock().unwrap();
    let tx = map.get_mut(&tid).and_then(|o| o.as_mut()).expect("no active TX");
    f(tx)
}
```

The `with_tx` helper is the only entry point for state access in every backend. Changing
its implementation (under `#[cfg(feature = "simulation")]`) is the **only** change needed
in each backend — no algorithm modifications.

### Required changes per backend

1. In `runtime-core`/`runtime-tinystm`/etc.:
   - Add `#[cfg(not(feature = "simulation"))]` branch for `thread_local!` storage
   - Add `#[cfg(feature = "simulation")]` branch for `HashMap` storage
   - Add `ThreadId` type (wraps `u64`) + `set_sim_thread_id()` / `clear_sim_thread_id()`
   - Add `Serialize`/`Deserialize` to `TxState` and all reachable types
   - Add `snapshot_all_tx_states()` / `restore_tx_states()` for checkpointing

2. In each backend (`norec`, `tl2`, `tinystm`, `swisstm`, etc.):
   - The `with_tx()` function is already the sole accessor for TxState
   - Switch its implementation based on `cfg(feature = "simulation")`
   - Everything else in the backend stays identical

3. The simulator's `Cargo.toml` declares:
   ```toml
   [dependencies]
   runtime-norec = { path = "../../expli_instr/rust/workspace/runtime/norec", features = ["simulation"] }
   runtime-tl2   = { path = "../../expli_instr/rust/workspace/runtime/tl2", features = ["simulation"], optional = true }
   # ... one per backend, optional, selected by CLI/feature
   ```

---

## 4. Thread Pool and Deterministic Scheduler

### 4.1 One Real Thread Per Simulated Thread

Each simulated thread maps to one real OS thread:

```
Simulated threads:   T0    T1    T2    T3
                     │     │     │     │
Real OS threads:    [R0]  [R1]  [R2]  [R3]
                     │     │     │     │
State:             HashMap<SimThreadId, TxState>
```

At any moment, at most one real thread is executing (deterministic interleaving).
The others are blocked on a `Barrier` or `Condvar`.

### 4.2 Scheduler Algorithm

```
1. Load trace events into a global timestamp-ordered queue.
2. Fan events out to per-thread queues.
3. Scheduler loop:
   a. Pick the thread with the next event (lowest timestamp, then thread_id).
   b. Wake that thread.
   c. Thread processes exactly one event (calls backend function).
   d. Thread signals done, blocks on barrier.
   e. Verifier checks result.
   f. If checkpoint due, serialize all TxState from HashMap.
   g. Repeat until queue empty.
```

### 4.3 Integration with Backend API

```
Event type            → Backend call
───────────────────────────────────────────
TxBegin               → tm_begin()
TxEnd                 → tm_commit()
Read { addr, width }  → tm_read_u32(addr as *mut u32)
Write { addr, val }   → tm_write_u32(addr as *mut u32, val as u32)
Alloc { addr, size }  → (tracked by verifier, not backend)
Free { addr }         → (tracked by verifier)
```

### 4.4 Checkpoint/Restore

At a checkpoint event (or periodically):
```
1. Barrier: all threads blocked.
2. Read HashMap<SimThreadId, TxState> from simulation storage.
3. Serialize: { clock, event_queue, thread_map, shadow_memory, verifier_state }.
4. Write to file (bincode).
```

Restore:
```
1. Deserialize checkpoint file.
2. Restore HashMap<SimThreadId, TxState> into simulation storage.
3. Rebuild per-thread event queues from remaining events.
4. Resume scheduling from checkpoint timestamp.
```

---

## 5. Verifier

Runs in the main scheduler thread between events (not inside the backend).

| Check | Mechanism |
|-------|-----------|
| **Double-free** | Shadow memory tracks `addr → (size, is_freed)`. Free checks. |
| **Use-after-free** | Read/write to freed address flagged. |
| **Non-TM address** | Read/write to address outside TM region flagged. |
| **Out-of-TX access** | Read/write without active begin flagged. |
| **Opacity** | Check read-set at commit (versions unchanged). |
| **Money conservation** | Compare sum of committed values before/after trace. |
| **Deadlock** | Wait-for graph built from backend's lock state + scheduler's blocked-thread info. |

### Deadlock Detection

Since all threads run one-at-a-time under scheduler control, the scheduler knows
exactly which threads are blocked and what they're waiting for:

```rust
struct DeadlockDetector {
    /// Threads currently waiting (blocked on a lock acquire inside the backend).
    waiting: HashMap<SimThreadId, WaitReason>,
    /// Resources held per thread (queried from backend after each event).
    holds: HashMap<SimThreadId, Vec<ResourceId>>,
}
```

On each scheduler tick:
1. If a thread spent more than `N` spin iterations in `tm_begin`/`tm_read`/`tm_write`:
   record as `waiting`.
2. Build wait-for graph: `waiting[t]` waits for resource `r`; `holds[u]` contains `r`.
3. DFS cycle detection.
4. On cycle: abort the lowest-priority thread in the cycle.

---

## 6. Trace Sources

### 6.1 LLVM Plugin --emit-tm-trace

Add to the LLVM cleanup pass. For each instrumented call (`tm_begin`, `tm_read_*`, etc.),
emit a JSONL line with synthetic timestamp + thread_id + DWARF source location.

### 6.2 C++ Runtime TM_TRACE_PATH

Environment variable `TM_TRACE_PATH=/tmp/mytrace.raw`. When set, the C++ hook layer
(`tm_hooks.cpp`) writes one line per TM operation to the file. The existing
`tm-trace2jsonl` binary converts this raw format to JSONL.

---

## 7. Implementation Phases

| Phase | What | Key Files |
|-------|------|-----------|
| **P1** | `simulation` feature in `runtime-core`: `HashMap`-based TxState storage, `ThreadId`, `set_sim_thread_id()`, `Serialize` on TxState | `expli_instr/rust/workspace/runtime/core/src/lib.rs` |
| **P2** | `simulation` feature in one backend (norec): switch `with_tx()` under `#[cfg]` | `expli_instr/rust/workspace/runtime/norec/src/lib.rs` |
| **P3** | Simulator scaffold: thread pool, per-thread queues, one-at-a-time scheduler | `simulator/src/scheduler.rs`, `simulator/src/thread_pool.rs` |
| **P4** | Trace replay: feed events through scheduler → actual backend calls | `simulator/src/replay.rs` |
| **P5** | Verifier: shadow memory, double-free, out-of-TX, opacity | `simulator/src/verifier.rs` |
| **P6** | **DONE** Deadlock detector: wait-for graph, cycle detection (per-thread retry tracking, conflict-graph DFS, report) | `simulator/src/deadlock.rs` |
| **P7** | **DONE** Checkpoint/restore with backend state serialization via bincode (opaque blob per backend, save/load file, snapshot_engine/restore_engine) | `simulator/src/checkpoint.rs` |
| **P8** | Add simulation feature to remaining backends (tl2, tinystm, swisstm, ...) | all `runtime-*/src/lib.rs` |
| **P9** | Trace sources: LLVM --emit-tm-trace + C++ TM_TRACE_PATH hook | `plugin/passes/*`, `backends/tm_impl/common/tm_hooks.cpp` |

---

## 8. CLI

```
tm-sim [--backend norec|tl2|tinystm|swisstm|sgl|...]
       [--trace trace.jsonl]
       [--checkpoint checkpoint.bin]
       [--checkpoint-every N]
       [--seed S]
       [--max-events M]
       [--deadlock-timeout D]
```

Backend selection: at compile time via Cargo features, at runtime by `--backend` arg
(matching a [conditional compilation] enum).

---

## 9. Risks

| Risk | Mitigation |
|------|------------|
| Thread context-switching overhead for DES | Acceptable for debugging (not production perf). Use `std::thread::yield_now()` + spin for tight scheduling. |
| Backend uses `thread_local!` beyond `with_tx()` | Audit each backend. The three TinySTM variants, TL2, NOrec, SwissTM all route through `with_tx()`. |
| Serialize on TxState is complex (pointers, Vecs) | Derive `Serialize`/`Deserialize` via serde. TxState contains only `Vec`, `HashMap`, `u64`, `bool` — all trivially serializable. |
| Simulation mode diverges from production | Run same trace through real multi-threaded backend and simulator; compare abort rates and final state. |
