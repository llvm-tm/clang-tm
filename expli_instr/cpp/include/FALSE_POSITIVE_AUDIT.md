# Plugin False-Positive Instrumentation Audit

## Problem
The LLVM TM pass instruments every load/store in a TX function whose pointer
argument traces to a TM-annotated global.  If the trace analysis is too
conservative, the pass will instrument accesses to **non-TM memory** (stack
locals, heap outside the TM pool), adding overhead without benefit.

## Approach
Track every `tm_read`/`tm_write` call at runtime and classify the target
address as either **in-TM-pool** (correct) or **outside-TM-pool**
(false-positive).  Report summary at exit.

## Implementation Sketch

### 1. Register TM-allocated regions
Add a global table that records every range allocated via `tm_malloc`,
`tm_calloc`, `tm_realloc`:

```c
// runtime/tm_pool.h
typedef struct { void *start; size_t size; } TMRange;
extern TMRange g_tm_pool[4096];
extern int     g_tm_pool_count;

void tm_pool_register(void *ptr, size_t size);
int  tm_pool_contains(const void *addr);
```

Hook into `tm_malloc`/`tm_calloc`/`tm_realloc` to register each allocation.

### 2. Instrument `tm_read`/`tm_write` counters
Add per-typed counters inside every backend's read/write functions:

```c
// Incremented on every tm_read/tm_write call
extern uint64_t g_tm_read_count;
extern uint64_t g_tm_write_count;
extern uint64_t g_tm_read_false_positive;
extern uint64_t g_tm_write_false_positive;
```

Inside `tm_read_i8` (for example):

```c
uint64_t tm_read_i8(uint64_t *addr) {
    g_tm_read_count++;
    if (!tm_pool_contains(addr)) g_tm_read_false_positive++;
    // ... actual TM read logic ...
}
```

### 3. Dump at exit
Call from `tm_exit`:

```c
void tm_dump_stats() {
    printf("TM-READS:          %lu\n",  g_tm_read_count);
    printf("TM-READS-FALSE:    %lu  (%.1f%%)\n",
           g_tm_read_false_positive,
           100.0 * g_tm_read_false_positive / g_tm_read_count);
    printf("TM-WRITES:         %lu\n",  g_tm_write_count);
    printf("TM-WRITES-FALSE:   %lu  (%.1f%%)\n",
           g_tm_write_false_positive,
           100.0 * g_tm_write_false_positive / g_tm_write_count);
}
```

### 4. Static analysis alternative
The **TMAudit** compile-time flag (`-DTMAudit=ON`) dumps every load/store in
every clone with "shared=Y/N traced=Y/N" classification (see
`instrumentLoadsStoresInFunction` in `tm_method_instrumentation.hpp`).

Run with:
```
cmake -DTMAudit=ON -B build
```

Look for `* LOAD` / `* STORE` audit lines — lines with `*` are false positives
(shared=N or traced=N).

## Interpretation
- **False-positive reads/writes** are accesses to addresses NOT in the TM pool.
  These add unnecessary overhead (extra function call, read-set insertion) for
  no correctness benefit.
- **False-negative reads/writes** are TM-pool addresses that are NOT
  instrumented (would cause data races / inconsistency).  These are the more
  dangerous category — the `TMInstrumentCheckPass` (`tm-instrument-check`)
  detects them statically.
- The ratio `false-positive / total-instrumented` measures pass precision.
  For STAMP benchmarks, this should be near 0% since all shared data should
  be in TM-annotated globals.
