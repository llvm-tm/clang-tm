# Simulator Improvement Plan — Modular Architecture

## Architecture Overview

The simulator now has a clean separation of concerns using **three independent input files**:

```
┌──────────────────────────────┐     ┌──────────────────────────────┐
│     Machine Profile (JSON)   │     │    Workload Profile (JSON)   │
│                              │     │                              │
│  Collected once per machine  │     │  From compiler plugin or     │
│  by running profiling script │     │  trace analysis              │
│                              │     │                              │
│  - CPU model + frequency     │     │  - Benchmark name            │
│  - TSX cycle costs           │     │  - Read/write-set sizes      │
│  - L1/L2/L3 latency          │     │  - Abort rate + breakdown    │
│  - Cache-line size           │     │  - Contention level          │
│  - Per-backend overhead      │     │  - Access pattern (locality) │
└──────────┬───────────────────┘     └──────────┬───────────────────┘
           │                                    │
           ▼                                    ▼
     ┌──────────────────────────────────────────────────────────┐
     │                   DES Engine (clock + cost)              │
     │  cost = f(event_kind, machine_profile, backend)         │
     │  workload validates/calibrates against WorkloadProfile  │
     └──────────────────────────────────────────────────────────┘
           ▲
           │
     ┌─────┴──────────────┐
     │   Trace Events     │  (from compiler plugin or trace2jsonl)
     │                    │
     │  - TxBegin/TxEnd   │
     │  - Read/Write addr │
     │  - Alloc/Free      │
     │  - Abort           │
     └────────────────────┘
```

## Key Design Principles

1. **Profiling data is separate from workload data**: The machine profile (JSON) contains only hardware characteristics and can be generated on one machine and consumed on another. This enables cross-machine comparison and validation.

2. **Workload profile is separate from hardware profile**: The workload describes what the application *does* — read-set sizes, write-set sizes, contention — independently of what hardware it runs on. The compiler plugin (LLVM pass) can provide this information.

3. **Cost model is backend-aware**: Different TM backends (TinySTM, NOrec, TL2, TSXSGL, etc.) have different cost formulas for the same event. The cost model selects the right formula based on the `BackendProfile`.

4. **Clock drives two modes**: The DES engine can advance the clock by matching trace timestamps (original correctness-only mode) or by accumulating estimated cycle costs from the machine profile (cost mode for what-if analysis).

## File Structure

```
simulator/src/
  machine_profile.rs   — MachineProfile struct + JSON I/O
  workload_profile.rs  — WorkloadProfile struct + JSON I/O
  cost_model.rs        — EventKind → cycle cost (param by MachineProfile)
  calibration.rs       — Profiling data → MachineProfile
  engine.rs            — DES loop with clock-advancement modes
  backend.rs           — Backend enum (includes TsxSim)
  ...

patches/profile/tsx/
  0001-tsxsgl-tsx-timing-instrumentation.patch   — RDTSC instrumentation
  run_tsx_profiling.py                            — Experiments runner
  run_workflow.sh                                 — End-to-end workflow
  results/                                        — CSV profiling results
  calibration/                                    — Calibration JSON + MachineProfile JSON

explicit_api/rust/workspace/runtime/tsx_sim/
  src/lib.rs         — TSX simulation backend (bloom + write-set + conflict)
  Cargo.toml
```

## Phases

### Phase 1: Profiling Infrastructure ✅
- `patches/profile/tsx/0001-tsxsgl-tsx-timing-instrumentation.patch` — RDTSC instrumentation
- `run_tsx_profiling.py` runs benchmarks, parses TSX_STATS, generates:
  - `results/tsx_profile_{timestamp}.csv` — raw results
  - `calibration/calibration_{timestamp}.json` — per-benchmark calibration
  - `calibration/machine_profile_{timestamp}.json` — hardware profile (portable)

### Phase 2: Cost Model ✅
- `MachineProfile` struct with serde JSON serialization
- `WorkloadProfile` struct with serde JSON serialization  
- `cost_model.rs` — `event_cost(kind, &machine, backend)` → cycles
- `CalibratedCostModel` — pre-computed per-event costs for fast dispatch
- `estimate_workload()` — predict total execution time from workload + machine

### Phase 3: TSX Simulation Backend ✅
- `runtime/tsx_sim/` — cache-line write-set, bloom-filter read-set
- Conflict detection on commit (bloom may-match for reads, exact for writes)
- Capacity abort simulation (configurable limits)
- Virtual cycle counter accumulated per operation

### Phase 4: Cost-Mode DES Engine ✅
- `engine.rs` — `ClockMode::Timestamp` (trace match) and `ClockMode::Cost` (accumulate)
- `print_summary()` — commits, aborts, abort rate, estimated cycles

### Phase 5: Validation (pending)
- Run same traces through real TSXSGL and TSX-SIM backend
- Compare commit/abort decisions (must match exactly)
- Compare estimated vs real execution time
- Error metrics: MAPE, RMSE, correlation coefficient

### Phase 6: Calibration from real hardware (pending)
- Apply profiling patch on RTM-capable machine
- Run `run_workflow.sh` to collect TSX_STATS + generate machine profile
- Import machine profile JSON into simulator for calibrated cost estimates

## Machine Profile JSON Format

```json
{
  "cpu": "Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz",
  "freq_ghz": 3.7,
  "tsx": {
    "xbegin_cycles": 18.5,
    "xend_cycles": 75.2,
    "xabort_cycles": 1520.0,
    "read_l1_cycles": 5.8,
    "write_l1_cycles": 6.2,
    "bloom_check_cycles": 2.0,
    "mutex_lock_cycles": 95.0,
    "mutex_unlock_cycles": 85.0,
    "conflict_abort_penalty": 2000.0,
    "cache_line_size": 64,
    "max_read_lines": 512,
    "max_write_lines": 128
  },
  "memory": {
    "l1_hit_cycles": 4.0,
    "l2_hit_cycles": 12.0,
    "l3_hit_cycles": 40.0,
    "ram_cycles": 200.0
  },
  "backends": [
    {
      "backend": "default",
      "begin_overhead": 18.5,
      "commit_overhead": 75.2,
      "abort_overhead": 1520.0,
      "read_overhead": 2.0,
      "write_overhead": 2.0,
      "validation_entry_cost": 3.0,
      "lock_acquire_cost": 95.0
    }
  ],
  "collected": "2026-06-21T...",
  "description": "Auto-generated from N profiling runs on ..."
}
```

## Workload Profile JSON Format

```json
{
  "benchmark": "fuzz_counter",
  "threads": 4,
  "duration_s": 5.0,
  "total_transactions": 100000,
  "read_set": {
    "mean": 4.2, "median": 4, "p90": 6, "p99": 10,
    "distribution": { "poisson": { "lambda": 4.2 } }
  },
  "write_set": {
    "mean": 2.1, "median": 2, "p90": 4, "p99": 6,
    "distribution": { "poisson": { "lambda": 2.1 } }
  },
  "access_pattern": {
    "read_ratio": 0.67,
    "locality": 0.3,
    "stride": 1.0
  },
  "contention": {
    "abort_rate": 0.12,
    "conflict_abort_ratio": 0.7,
    "capacity_abort_ratio": 0.05,
    "level": "medium"
  },
  "backends": [
    {
      "backend": "tinystm",
      "validations_per_commit": 1.2,
      "contentions_per_commit": 0.3,
      "extensions_per_tx": 0.1
    }
  ]
}
```

## CLI Usage (tm-des)

```
# Original mode: correctness checking (no cost estimation)
tm-des --trace trace.jsonl

# Cost mode with default machine profile
tm-des --trace trace.jsonl --clock-mode cost

# Cost mode with loaded machine profile
tm-des --trace trace.jsonl --clock-mode cost \
       --machine-profile machine_profile.json \
       --backend tsxsgl

# Cost mode with workload calibration
tm-des --trace trace.jsonl --clock-mode cost \
       --machine-profile machine_profile.json \
       --workload-profile workload.json
```

## Known TSX Constraints (Intel Skylake)
- Read-set capacity: ~28-40 KB (L1 cache size dependent, ~464-512 cache lines)
- Write-set capacity: ~8-12 KB (~128-192 cache lines)
- Max transaction duration: ~100K CPU cycles before abort (configurable in MSR)
- Capacity abort when read-set or write-set overflows L1
- Conflict abort on any cache-line invalidation from another core
- Self-abort via _xabort()
- Explicit abort cost: ~1500 cycles (restore architectural state)
- Successful _xend cost: ~100 cycles (commit store buffer)
