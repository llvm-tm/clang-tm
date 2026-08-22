# gem5 TM Simulation

Cycle-accurate simulation of TM benchmarks (bank, STAMP, etc.) on machines
without hardware TSX/TME/HTM support. gem5 simulates the missing ISA extension.

## Quick Start

```bash
# 1. Clone + build gem5 (~15 min, one-time)
./gem5_sim/setup.sh

# 2. Build the bank benchmark for gem5 (static x86-64 via Docker)
docker run --rm --platform linux/amd64 \
  -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 \
  sh -c "apk add --no-cache g++ make >/dev/null && \
         make BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl"

# 3. Run SE-mode simulation
./gem5_sim/scripts/run-bank-gem5.sh TSXSGL 1 64 2000

# 4. Verify output
#    Should print: PASS: Money conserved
#    ROI stats in gem5_sim/m5out/bank-tsxsgl-t1-a64-n2000/stats.txt
```

## What This Provides

- **x86 TSX** (Intel RTM): `XBEGIN`/`XEND`/`XABORT`/`XTEST` via gem5 in-tree support
- **ARM TME**: `TSTART`/`TTEST`/`TCOMMIT`/`TCANCEL` via gem5 merged support
- **POWER8 HTM**: `tbegin.`/`tend.`/`tabort.` via this repo's implementation
- **Software STM cross-checks**: NOrec, TinySTM, TL2 run on the same simulated hardware

All simulations use the calibrated `MESI_Three_Level_HTM` Ruby cache hierarchy
with Broadwell-EP timing parameters (xbegin=60 cycles, xend=178 cycles).

## Directory Layout

```
gem5_sim/
  setup.sh              # Clone + build gem5
  .gem5-version          # Pinned gem5 version
  gem5/                  # (gitignored) gem5 source + build
  configs/               # gem5 simulation configs
    x86-se-bank.py       # SE-mode bank benchmark
    components/          # Ruby MESI_Three_Level_HTM hierarchy
  scripts/               # Build & run helpers
    run-bank-gem5.sh     # Build-via-Docker + run + verify
  patches/               # x86 TSX patches (reference)
  workloads/             # HTM benchmark sources (rtm, tme, power8)
  docs/                  # Build guide, calibration, workflows
  m5out/                 # (gitignored) simulation output
```

## Build Targets

```bash
make gem5              # Clone + build gem5 (X86_TSX target)
make gem5-clean        # Remove gem5 build artifacts
```

## Scripts

| Script | Purpose |
|--------|---------|
| `setup.sh` | Clone gem5 + build X86_TSX |
| `setup.sh --clone` | Clone only |
| `setup.sh --build` | Build only |
| `setup.sh --clean` | Clean build |
| `setup.sh --build-all` | Build ALL ISAs |
| `scripts/run-bank-gem5.sh` | Run bank benchmark under gem5 |
| `scripts/compare_gem5_tsxsim.py` | Compare gem5 vs tsx-sim cost model |

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `GEM5_VERSION` | v25.1.0.1 | gem5 tag to checkout |
| `GEM5_JOBS` | nproc | Parallel build jobs |
| `GEM5_TARGET` | X86_TSX | ISA build target |
| `GEM5_DIR` | `gem5_sim/gem5` | gem5 source location |
| `TM_API` | `.` (repo root) | tm_api_cpp repo location |
| `CLK` | 1.8GHz | Simulated core clock |
| `CPU_TYPE` | timing | CPU model (timing/atomic/o3) |
| `TRACE` | 0 | Set to 1 for TM trace capture |

## Benchmarks

Build any benchmark with `GEM5=1`:

```bash
# bank (primary benchmark)
make -C benchmarks/cpp BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl

# fuzz_counter
make -C benchmarks/cpp BACKEND=TSXSGL GEM5=1 bin/fuzz_counter_gem5_tsxsgl
```

The `GEM5=1` flag enables:
- `-DGEM5_M5OPS` -- m5 pseudo-instruction ROI markers
- `-DTM_REGION_SIZE=536870912` -- 512 MiB TM region for simulated address space
- Static x86-64 linking (via musl/Alpine Docker)

## References

- [gem5 TSX calibration guide](docs/gem5-tsx-calibration.md)
- [Detailed build instructions](docs/build.md)
- [Simulation workflows](docs/workflow.md)
- [x86 TSX patch details](docs/x86-tsx-patch.md)
