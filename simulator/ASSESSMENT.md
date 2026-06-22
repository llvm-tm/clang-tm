# Simulator Assessment & Improvement Plan

## Current State

The simulator has two execution paths:

| Path | Binary | Engine | Backend execution | Contention modeling |
|------|--------|--------|-------------------|---------------------|
| Real-backend replay | `tm-sim` | `SimEngine` | Runs real TM backend (NOrec, TL2, TinySTM, Romulus, SwissTM, tsx-sim) under both `--clock-mode timestamp` and `--clock-mode cost` | Yes — real read-set/write-set tracking, validation, aborts |
| Abstract DES | `tm-des` | `SimState` | Process events from a queue, advance clock. No backend execution. Cost mode just accumulates per-event constants. | No — cost mode is additive only, cannot detect conflicts |

The `tm-des` cost mode (`SimState`) has a known limitation: **contention is not modeled**. Multi-threaded throughput is flat across thread counts because every transaction pays the same cost regardless of conflicts, retries, or SGL fallback.

For contention-aware estimation, use `tm-sim --clock-mode cost` which runs the real backend while accumulating calibrated cycle costs. The backend detects aborts naturally and retries add cost, so throughput varies correctly with thread count.

### Comparison Results (from `tools/compare_tsxsgl.py`)

Benchmarks on Intel Xeon E5-2648L v4 (Broadwell-EP, 1.8 GHz nominal, RTM available).
Using `tm-sim --clock-mode cost` with real TSXSGL backend data.

The `SimState` cost mode (no backend) produces flat throughput across thread counts,
which diverges from real benchmarks by +44% to +305%.

With `SimEngine` cost mode, single-threaded accuracy is within <10% after calibration,
and multi-threaded trends (throughput drop with thread count) are captured correctly.

## Known Issues

### 1. SimState cost mode is contention-unaware

`tm-des --clock-mode cost` (`SimState::dispatch()`) does NOT execute any TM backend.
It simply accumulates per-event cycle costs from the machine profile.
Every transaction pays the same cost regardless of:
- Read-write conflicts between concurrent transactions
- TSX capacity aborts (read-set/write-set overflow)
- SGL fallback after retry exhaustion
- sigsetjmp/siglongjmp retry overhead
- Cache-line invalidation traffic

**Workaround**: Use `tm-sim --clock-mode cost` for contention-aware estimation.
**Long-term fix**: Wire a Backend into SimState so it can detect conflicts in cost mode.

### 2. Hardcoded costs in tsx-sim Phase 4 retry loop

The `flush_pending_begins()` method in `SimEngine` previously hardcoded
xbegin=60, xabort=1500, sgl_lock=75 cycle costs instead of using the
calibrated cost model. This has been fixed — costs are now read from
`CalibratedCostModel` when available.

### 3. No workload profile integration

The `WorkloadProfile` struct was defined with serde, load/save, and
`estimated_cycles_per_tx()` but was never wired into any binary or
loaded from the CLI. It has been removed. Analytical workload profiling
can be re-added when a specific use case arises.

### 4. Machine profile calibration

The `machine_profiles/skylake.json` and `broadwell_ep_v4.json` profiles
contain estimated cycle costs. For best accuracy:
- Run `patches/profile/tsx/run_tsx_profiling.py` on the target hardware
- This generates a calibration JSON and machine profile from real TSX_STATS
- Pass the result via `tm-sim --machine-profile <path> --clock-mode cost`

Current profiles are estimated from Broadwell-EP measurements and may
under- or over-estimate on other CPUs.

## Improvement Plan

### P0 — Wire real backend into SimState (tm-des cost mode)

Currently `SimState::dispatch()` doesn't execute any TM backend.
Change it so that `--clock-mode cost` in `tm-des` runs a real backend
(like `SimEngine` does), detecting aborts naturally while advancing
the clock by calibrated cycle costs.

This would make `tm-des` contention-aware like `tm-sim`, and eliminate
the flat-throughput problem.

### P1 — Fidelity CI gate

Add a step in CI that runs `compare_sim.py` or `tools/tsx_abort_compare.py`
and fails if the error exceeds a threshold (e.g., >50% for single-threaded,
>100% for multi-threaded). This prevents regressions in the cost model.

### P2 — Cross-backend fidelity sweep

Run a systematic comparison of all 6 simulated backends against their
real C++ counterparts:
- NOrec (calibrated once in 2026-06-22 session)
- TL2, TinySTM, Romulus, SwissTM (never calibrated)
- tsx-sim (calibrated against TSXSGL, +44% to +305% error documented)

Generate a fidelity table like:

| Backend | Benchmark | 1t error | 4t error |
|---------|-----------|----------|----------|
| NOrec   | fuzz_counter | ? | ? |
| TL2     | fuzz_counter | ? | ? |
| ...     | ...       | ...      | ...      |

### P3 — Account for retry overhead in event replay

When a transaction aborts in `SimEngine`, the event trace contains a
single TxBegin + body + Abort. The retry loop cost (re-issuing TxBegin,
possible SGL spin-wait) is not reflected. Add a configurable retry-cost
multiplier on abort events.

### P4 — Effective CPU frequency detection

The machine profile stores nominal frequency (e.g., 1.8 GHz for Broadwell-EP).
Turbo boost raises single-core bursts to ~2.8–3.0 GHz, making
cycles→time conversions inaccurate. The profiling patch collects
RDTSC measurements — wall time / cycles gives effective frequency.

### P5 — SGL fallback costing transition

When the tsx-sim backend falls back to SGL (after max retries), the
cost model should switch from TSX costs to SGL costs:
- xbegin → mutex_lock_cycles
- xend → mutex_unlock_cycles
- reads/writes within SGL: same as normal but without TSX overhead
- Spin-wait overhead when lock contended

The `CalibratedCostModel` already has `sgl_begin_cost`/`sgl_end_cost`
fields — they just need to be used conditionally in `flush_pending_begins`.

### P6 — Dynamic cost calibration

When running with `--clock-mode cost` alongside `--machine-profile`,
allow the machine profile to specify per-event costs derived from
profiling sweeps. The `calibration.rs` module already parses TSX_STATS
into `CalibrationRecord` — wire this through so the simulator can load
a calibration JSON and compute event costs directly.

## Summary

| Priority | Fix | Impact |
|----------|-----|--------|
| P0 | Wire backend into SimState cost mode | Fixes flat multi-threaded throughput in tm-des |
| P1 | Fidelity CI gate | Prevents cost model regressions |
| P2 | Cross-backend fidelity sweep | Quantifies accuracy for all 6 backends |
| P3 | Retry overhead cost | Improves accuracy under high contention |
| P4 | Effective frequency | Improves cycles→time conversion |
| P5 | SGL fallback costing | Accurate cost when TSX capacity exceeded |
| P6 | Dynamic calibration | Zero-config per-machine accuracy |
