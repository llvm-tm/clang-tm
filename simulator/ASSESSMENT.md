# Simulator Assessment & Improvement Plan

## Current State

The DES engine (`tm-des`) supports two clock modes:

- **`--clock-mode timestamp`**: runs the real TM backend (NOrec, TL2, TinySTM)
  to detect conflicts/aborts deterministically. Clock = trace timestamp.

- **`--clock-mode cost`**: runs *no backend* — just accumulates per-event
  cycle costs from the `CalibratedCostModel`. Clock = Σ event_cost(kind).

The cost mode was compared against real TSXSGL benchmarks on an Intel
Xeon E5-2648L v4 (Broadwell-EP, 1.8 GHz nominal, RTM available).

## Comparison Results

Tool: `tools/compare_tsxsgl.py`

| Benchmark | Threads | Real TPS | Sim TPS | Error | Sim cyc/txn |
|-----------|---------|----------|---------|-------|-------------|
| fuzz_counter | 1 | 10.0M | 14.4M | **+44%** | 125 |
| fuzz_counter | 2 | 7.7M | 14.4M | **+87%** | 125 |
| fuzz_counter | 4 | 5.6M | 14.4M | **+156%** | 125 |
| fuzz_bank | 1 | 7.7M | 12.1M | **+57%** | 149 |
| fuzz_bank | 2 | 3.3M | 12.1M | **+269%** | 149 |
| fuzz_bank | 4 | 3.0M | 12.1M | **+305%** | 149 |

### Key observation

**Simulated throughput is flat across thread counts** — 14.4M for fuzz_counter,
12.1M for fuzz_bank regardless of 1/2/4 threads. Real throughput drops by 44%
(fuzz_counter 1t→4t: 10M→5.6M) and 61% (fuzz_bank: 7.7M→3.0M) due to
TSX contention (aborts, retries, SGL fallback). The cost mode does not model
contention at all — it's just a sum of per-event constants.

## Root Causes

### 1. Cost constants are under-estimated

The machine profile hardcodes:
```json
"xbegin_cycles": 20,    "xend_cycles": 80,
"read_l1_cycles": 4,    "write_l1_cycles": 5,
"mutex_lock_cycles": 50, "mutex_unlock_cycles": 50
```

Real TSX_STATS profiling (from the profiling patch) measured:
- xbegin_ok: **60** cycles (fuzz_counter 1t) vs 20 in profile
- xend: **178** cycles vs 80
- read: **64** cycles vs 4 (but includes function call + RDTSC overhead)
- sgl_begin: **204** cycles vs 50

Even the "calibrated" profile uses idealized Skylake numbers, not the
actual measurements from this machine.

### 2. No contention model in cost mode

`--clock-mode cost` does NOT run the backend. It cannot detect:
- Read-write conflicts between concurrent transactions
- TSX capacity aborts (read-set/write-set overflow)
- SGL fallback after retry exhaustion
- sigsetjmp/siglongjmp retry overhead
- Cache-line invalidation traffic

Every transaction pays the same cost regardless of how many threads
are running or whether they conflict.

### 3. CPU frequency uncertainty

Nominal base frequency is 1.8 GHz, but turbo boost raises single-core
bursts to ~2.8-3.0 GHz. `/usr/bin/time` measures wall clock, not cycles.
Converting cycles→time via 1.8 GHz over-estimates real cycles/txn when
turbo is active (wider gap between sim and real).

### 4. Missing overheads

The per-txn cost model uses:
- fuzz_counter: begin(50) + read(5+4) + write(6+5) + end(80) = **150 cycles**
- fuzz_bank:   begin(50) + 2×read(2×9) + 2×write(2×11) + end(80) = **170 cycles**

Measured real per-txn cost:
- fuzz_counter 1t: **180 cycles** (30 cycles unaccounted)
- fuzz_bank 1t: **234 cycles** (64 cycles unaccounted)

Missing costs include: extern "C" function call overhead, sigsetjmp setup
per txn, TM framework dispatch (hook lookup), TLS variable access, memory
ordering fences, and branch mispredictions in the retry loop.

## Improvement Plan

### P0 — Calibrate cost model from real profiling data

Instead of hardcoding idealized Skylake numbers, derive costs from the
TSX_STATS output collected by `patches/profile/tsx/0001-tsxsgl-tsx-timing-instrumentation.patch`.

**What**: Update `machine_profiles/broadwell_ep_v4.json` to use measured
costs from the profiling sweep.

**Key values to use** (from TSX_STATS on this machine):
- xbegin_cycles: **60** (fuzz_counter 1t, non-contended)
- xend_cycles: **178** (fuzz_counter 1t)
- read_l1_cycles: **5** (subtracting RDTSC measurement overhead)
- write_l1_cycles: **6**
- mutex_lock_cycles: **50** (sgl_begin uncontended ~200 incl. RDTSC)
- mutex_unlock_cycles: **50**

Also add calibrated overhead entries to `BackendCharacteristics` for
function call overhead, retry-loop entry, etc.

**Expected impact**: Reduces single-threaded error from +44% to <10%.

### P1 — Run backend in cost mode

Currently cost mode skips the backend entirely. Change it so that:
- The backend IS executed (read-set/write-set tracking, validation, commit)
- The clock advances by event cost, NOT by timestamp
- Aborts are detected naturally by the backend
- Retries are simulated (re-issue TxBegin after abort, accumulating
  additional cost)

**What**:
```
SimState::dispatch():
  if clock_mode == Cost:
    advance_clock(event)       // accumulate cost
    run_backend(event)         // detect conflicts/aborts
```

This fixes the flat-multithreaded-throughput problem because aborts
and retries add cost. Implementations already exist in `sim_engine.rs`
(`dispatch`) — the cost mode just needs to call it.

**Expected impact**: Multi-threaded error drops from +87%/+269% to
within contention-modeling accuracy (~20-50%).

### P2 — Account for retry overhead

When a transaction aborts, the real benchmark does:
```c
while (!done) {
    tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
    tm_begin();
    if (tm_longjmp_ret != 0) continue;
    // ... body ...
    tm_end();
    done = 1;
}
```

Each retry costs: sigsetjmp + tm_begin (which may enter SGL + spin-wait).
The cost model currently charges TxBegin once; every retry charges again.

**What**: In `dispatch`, on Abort, push a synthetic TxBegin back onto
the event queue with the abort penalty added to the clock.

### P3 — Improve machine profile frequency tracking

**Option A**: Use actual RDTSC measurements from the profiling patch
rather than nominal CPU frequency. The `TSX_STATS` lines report
cycle counts directly — divide wall time by cycles to get effective
frequency.

**Option B**: The simulator could accept `--effective-ghz` to override
the machine profile's `freq_ghz`.

### P4 — Extended cost model for SGL fallback

When the backend falls back to SGL (after TSX retries exhausted), the
cost should switch from TSX costs to SGL costs:
- xbegin → mutex_lock_cycles (50)
- xend → mutex_unlock_cycles (50)
- reads/writes within SGL cost the same but without TSX overhead
- Spin-wait overhead when lock contended

The `CalibratedCostModel` already has `sgl_begin_cost`/`sgl_end_cost`
fields — they just need to be used conditionally.

### P5 — Dynamic cost calibration

When running with `--clock-mode cost` alongside `--machine-profile`,
allow the machine profile to specify per-event costs derived from
profiling sweeps. The `calibration.rs` module already parses TSX_STATS
into `CalibrationRecord` — wire this through so the simulator can load
a calibration JSON and compute event costs directly.

### P6 — Comparison tool improvements

- Run simulator in BOTH cost and timestamp modes, compare all three
  (real vs cost vs timestamp backend)
- Use the same iteration count in both real and sim runs
- Report per-txn cycles instead of (or in addition to) throughput
- Auto-detect effective CPU frequency from TSX_STATS or /proc/cpuinfo

## Summary

| Priority | Fix | Error before | Error after (est.) |
|----------|-----|-------------|-------------------|
| P0 | Calibrate costs from TSX_STATS | +44% (1t) | <10% |
| P1 | Run backend in cost mode | +87-305% (multi) | 20-50% |
| P2 | Account for retry overhead | part of P1 | additional 5-15% |
| P3 | Effective frequency | confounds P0 | — |
| P4 | SGL fallback costing | part of P1 | — |
| P5 | Dynamic calibration | manual config | auto-calibrated |
