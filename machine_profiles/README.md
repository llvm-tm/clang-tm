# Machine Profiles

Small curated JSON files, one per CPU model, describing hardware characteristics
that affect TSX execution time.  Each file is a `MachineProfile` consumed by the
simulator (`tm-des --machine-profile <file>`).

## How to add a new machine profile

1. Run the profiling workflow on the target machine:
   ```
   bash patches/profile/tsx/run_workflow.sh
   ```
2. The script generates `machine_profiles/machine_profile_{timestamp}.json`.
3. Rename it to `<cpu_model>.json` and commit.

## Files

| File | CPU | Source | Notes |
|------|-----|--------|-------|
| `skylake.json` | Intel Skylake (generic) | Literature estimates | Default; replace with real data |
| `machine_profile_*.json` | Auto-detected | Profiling patch | Temporary; rename after review |

## Contents

Each profile captures:

- **CPU**: model string, nominal frequency
- **TSX cycle costs**: xbegin, xend, xabort, L1 read/write — from RDTSC profiling
- **Memory latency**: L1/L2/L3/RAM cycles (used by non-TSX backends)
- **Backend overheads**: begin/commit/abort overhead, validation cost, lock acquisition cost
- **Capacity limits**: max cache lines in read-set/write-set before TSX capacity abort

## Precision

The profiling patch uses RDTSC, which measures wall-clock cycles.  Turbo boost,
frequency scaling, and out-of-order execution introduce variance.  The values
here are *approximate distributions*, not exact cycle counts.  Use the
simulator's cost model to run Monte Carlo experiments over the distribution.
