# Generic TM Fuzz Tool — Design Plan

## Goal

Take an arbitrary application (bitcode/binary), auto-instrument it at strategic TM points, stress with concurrent threads, collect traces, and assess correctness invariants — without requiring manual `[[tm::shared]]` annotations.

## Phase 1: Strategic Point Detection (LLVM Pass)

New pass `tm-fuzz-strategy` that auto-identifies instrumentation points:

| Point | Detection | Injection |
|-------|-----------|-----------|
| **Transaction boundaries** | Functions on `pthread_create` → `main` → `join` paths; loops with cross-iteration dependencies | `tm_begin`/`tm_end` + retry |
| **Shared data** | Globals accessed by ≥2 thread-reachable call graphs (reuse `tracesFromTMGlobal()`) | `tm_read_*`/`tm_write_*` |
| **Hot loops** | Profile-guided (instrument first run, re-instrument hot blocks) | Periodic sampling checks |
| **Sync points** | `pthread_mutex_lock/unlock`, atomic builtins | Record hold times |

Output: `!tm.strategic` metadata on candidate instructions.

## Phase 2: Sampling

Three runtime-switchable modes via `tm_swap_runtime("sampling")` or `TM_SAMPLE_RATE` env:

1. **Rate-limited** — every Nth access per thread. Per-thread counter in hook wrappers.
2. **Phase** — full instrumentation for first K transactions/thread, then sparse.
3. **Adaptive** — rate adjusts based on `tm_abort_count` / contention metrics.

Implementation: `sample_thunk` wrappers in `tm_hooks.cpp` that interpolate between stub and real hooks.

```
void sample_read_i4(void *addr, intptr_t val) {
    thread_local uint64_t cnt;
    if (++cnt % g_sample_rate == 0)
        real.read_i4(addr, val);
}
```

## Phase 3: Trace Collection

Extend existing `TM_TRACE_PATH` / `--emit-tm-trace`:

| Event | Record fields |
|-------|--------------|
| tx_begin | `ts tid txid` |
| tx_commit | `ts tid txid reads writes` |
| tx_abort | `ts tid txid reason` |
| read/write | `ts tid txid addr width val contention_flag` |
| malloc/free | `ts tid ptr size` |

## Phase 4: Invariant Assessment

Two modes:
1. **Sequential baseline** — single-thread stub run records all effects. Multi-threaded trace compared against it. Divergence = bug.
2. **User oracle** — simple callback (e.g. `sum(accounts)==C`). Framework calls it after each tx or at exit.

Extend `simulator/` to validate sequential consistency per address, detect lost updates, write skew, and contention heatmaps.

## Phase 5: `tm-fuzz` Driver CLI

```
tm-fuzz --app myapp.bc --threads 2,4,8 --sample-rate 10 \
        --strategy auto --invariant "sum(accts)==C" \
        --duration 60 --backend TINYSTM,NOREC
```

Pipeline: `opt` passes → run → `tm-trace2jsonl` → `tm-check` → report.

## Implementation Order

1. Rate-limited sampling wrappers in `tm_hooks.cpp` (~2d)
2. Strategic-point auto-detection pass (~1w)
3. Extended trace fields + simulator upgrades (~3d)
4. Sequential baseline comparison mode (~3d)
5. `tm-fuzz` driver script (~2d)

Total: ~3 weeks for prototype.
