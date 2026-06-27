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

## Completed Items

### ✅ P0 — Cross-LP conflict detection (SimState cost mode)

`SimState::dispatch()` now detects cross-LP address conflicts in cost mode:
- `in_flight_writes` / `in_flight_reads` maps track per-LP address sets
- On `Read { addr }`: checks `in_flight_writes` for Write-After-Read conflict → aborts the writer
- On `Write { addr }`: checks `in_flight_reads` for Read-After-Write conflict → aborts the reader
- On conflict, the abort cost plus retry penalty is added to `estimated_cycles`
- Aborted LP's `in_tx` is cleared; subsequent events are no-ops until next `TxBegin`
- TxEnd after synthetic abort doesn't count as a commit

This eliminates the flat-throughput problem — contention scales with thread count.

### ✅ P3 — Retry cost multiplier

- `SimState.retry_cost_multiplier` controls the retry penalty (default: 3x)
- On synthetic conflict: charges `abort_cost × retry_cost_multiplier` extra cycles
- Exposed via `--retry-cost-multiplier` CLI argument in `tm-des`
- Accounts for TX body re-execution cost after an abort

### ✅ P5 — SGL fallback costing (SimEngine)

- `SimEngine.sgl_mode: HashMap<u64, bool>` tracks per-thread SGL fallback state
- Set in `flush_pending_begins()` when `force_sgl()` is called (after max retries)
- `process_event()` checks `sgl_mode` before charging `TxEnd` cost: uses `sgl_end_cost`
  instead of `tx_end_cost` when in SGL mode
- Cleared on TxEnd or Abort via `sgl_mode.remove(&tid)`

### ✅ P4 — Effective frequency override

- `effective_freq_ghz` field on `SimState` overrides machine_profile's nominal frequency
- Exposed via `--effective-freq` CLI arg (e.g., `--effective-freq 2.5`)
- Used in `print_summary()` for cycles→time conversion
- Auto-computed from calibration data when available

### ✅ P6 — Dynamic calibration

- `--calibration` CLI arg loads a calibration JSON (map of benchmark → CalibrationRecord)
- Converted to `MachineProfile` via `calibration_to_machine_profile()`
- Overrides `--machine-profile` when both are provided
- Calibration records can contain measured cycle costs from profiling sweeps

### ✅ P1 — Fidelity CI gate

- `nightly.yml` fidelity-regression job now includes a Python gate step
- After `compare_real_sim.py` completes, parses the CSV `Fidelity` column
- Computes average fidelity across all benchmarks
- Fails the job if average fidelity < 50%
- Uploads the full fidelity results as an artifact for manual inspection

## Remaining

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

This requires running real C++ benchmarks with tm-trace enabled on
hardware with clang++-22. Run on CI or lab machine:

```sh
python3 simulator/compare_real_sim.py --backends NOREC,TL2,TINYSTM \
    --benchmarks bank,vacation,kmeans --csv fidelity_sweep.csv
```

The nightly.yml job does this automatically and the fidelity gate
prevents regressions. The full sweep across all 6 backends needs
manual infrastructure — add --backends ROMULUS,SWISSTM to the sweep
once their Rust sim backends are calibrated. The `--calibration`
flag can load measured costs from lab profiling data.

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
