# Live-App Simulation Plan

## Problem

Trace replay gives fixed execution paths. For bug finding and schedule
exploration we need to re-run the application under different interleavings.

## Architecture

```
┌────────────────────────────────────────────────────────┐
│                   Simulator (Rust)                      │
│                                                         │
│  ┌────────────────────────────────────────────────┐    │
│  │  SimBackend (C++ .so via dlopen)               │    │
│  │  Implements the hook table (tm_hooks.cpp)      │    │
│  │  Yields to scheduler on each TM operation      │    │
│  │                                                 │    │
│  │  ┌──────────────┐  ┌──────────────┐            │    │
│  │  │ Non-TSX:     │  │ TSX:         │            │    │
│  │  │ tm_read/write│  │ tm_begin/end │            │    │
│  │  │ → yield +    │  │ → tsx_sim    │            │    │
│  │  │   shadow mem │  │   conflict   │            │    │
│  │  │   conflict   │  │   detection  │            │    │
│  │  └──────────────┘  └──────────────┘            │    │
│  └───────────────────────┬────────────────────────┘    │
│                          │ yield on TM op              │
│  ┌───────────────────────▼────────────────────────┐    │
│  │  Deterministic Scheduler                       │    │
│  │                                                │    │
│  │  Each thread = stackful co-routine             │    │
│  │  (boost::context fiber). At each TM op:        │    │
│  │  1. Record event + advance virtual clock       │    │
│  │  2. Run conflict detection on write-set        │    │
│  │  3. If abort → rewind to tm_begin() (retry)    │    │
│  │  4. Select next ready co-routine               │    │
│  │                                                │    │
│  │  Clock: per-event cycle costs from calibrated  │    │
│  │  machine profile (same as cost mode).          │    │
│  └──────┬─────────────────────────────────────────┘    │
│         │                                              │
│  ┌──────▼─────────────────────────────────────────┐    │
│  │  Application (C++, compiled as .so with the    │    │
│  │  same source as the real benchmark).           │    │
│  │                                                │    │
│  │  Linked against SimBackend hook table.         │    │
│  │  Non-TM code runs at native speed (no yield).  │    │
│  └────────────────────────────────────────────────┘    │
└────────────────────────────────────────────────────────┘
```

### TSX mode

TSX aborts if reads/writes are instrumented (the instrumentation breaks
the hardware transaction). In TSX mode the SimBackend only intercepts
`tm_begin()`, `tm_end()`, and `tm_abort()`. At commit time it delegates
to the existing `tsx_sim` model (bloom filter read-set + cache-line
write-set + capacity abort simulation).

Real sequence:  `XBEGIN → (uninstrumented reads/writes) → XEND`
Sim sequence:    `tm_begin → (native reads/writes) → tm_end → tsx_sim`

### Non-TSX mode

Every `tm_read_*`/`tm_write_*` yields to the scheduler. The SimBackend
maintains per-thread read/write sets in shadow memory. The scheduler can
explore any interleaving at each TM op — this is the bug-finding path.

### Determinism

- Virtual clock advances by `cost_model.event_cost(kind)` per TM op.
- Scheduler PRNG controlling co-routine selection (same seed = same schedule).
- Shadow memory (`HashMap<u64, u64>`) for committed TM values.
- Non-TM memory is real (native speed) but deterministic given the same
  TM schedule because computation is deterministic given the same TM reads.

## Implementation phases

### Phase 1 — Single-thread proof of concept

1. Create `simulator/src/live_app/` directory.
2. Write `SimBackend.cpp` implementing the TM hook table:
   - Each hook calls a C FFI function in the Rust simulator
   - The Rust side records the event and returns control
3. Write a co-routine wrapper: the Rust simulator calls `dlopen("./app.so")`,
   calls `main()` via a `boost::context::fiber` that yields on each TM op.
4. Demonstrate: load the bank benchmark, run with a deterministic schedule,
   verify the invariant matches a real C++ run.

### Phase 2 — Multi-thread scheduler

1. Each application thread = one `boost::context::fiber`.
2. Round-robin: run one TM op per fiber, then switch.
3. Conflict detection: maintain write-set per fiber; on commit, check
   overlap with other active fibers' write-sets (matching tsx_sim logic).
4. Virtual clock advancement.

### Phase 3 — TSX integration

1. SimBackend TSX mode: `tm_begin()` starts a TSX region in the model.
2. `tm_end()` calls into Rust `tsx_sim` library to decide commit/abort.
3. On abort: inject retry (same as Phase 2 abort handling).
4. SGL fallback when capacity exceeded or abort limit reached.

### Phase 4 — Controlled schedule exploration

1. Instead of round-robin, use a schedule database to enumerate interleavings
   at write-set overlap points.
2. Combine with a property checker (invariant per-commit).
3. Report the minimal schedule that triggers each bug.

## Advantages over trace replay

| Aspect | Trace replay | Live-app simulation |
|--------|-------------|-------------------|
| Non-TM computation | `Computation{cycles}` events | Native speed, exact |
| Branch exploration | Fixed trace (one path) | Real branches per TM outcome |
| Schedule exploration | Interleaving fixed by trace | Any interleaving at each TM op |
| TSX coverage | tsx_sim on recorded trace | Real app logic with tsx commit model |
| Bug finding | Trace must contain the bug first | Can force interleavings to find bugs |
