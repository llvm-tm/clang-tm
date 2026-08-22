# Simulation Workflows

This document describes the standard simulation workflows for each TM
implementation.

## Workflow Overview

| TM Impl | Simulation Mode | CPU Model | Cache | Status |
|---------|----------------|-----------|-------|--------|
| POWER8 HTM | SE (bare-metal) | TimingSimpleCPU | Ruby MESI_Three_Level_HTM | Working |
| ARM TME | FS (KVM→Timing) or SE | TimingSimpleCPU | Ruby or Classic | Untested |
| x86 TSX | FS (KVM→O3) | O3 CPU | Ruby (patched) | Requires patch |

## POWER8 HTM

Fastest workflow — bare-metal SE mode, no OS boot needed:

```bash
# 1. Build gem5 (ALL target includes everything)
./scripts/build.sh all

# 2. Cross-compile benchmark
limactl shell debian
cd /path/to/workloads/power8
make array_sum_bench

# 3. Run
OUTDIR=./m5out/power8-htm ./scripts/run-power8-htm.sh
```

Debug options:
```bash
# HTM-specific debug
./build/ALL/gem5.opt --debug-flags=PowerHtm --outdir=./m5out ...

# All HTM debug (CPU + Memory + Power)
./build/ALL/gem5.opt --debug-flags=PowerHtm,HtmCpu,HtmMem --outdir=./m5out ...

# Full instruction trace
./build/ALL/gem5.opt --debug-flags=ExecAll --outdir=./m5out ...
```

## ARM TME

Requires KVM on Linux for fast-forward, or Atomic CPU fallback:

### With KVM (fast, Linux only):

```bash
./scripts/build.sh arm
./scripts/run-arm-tme.sh
```

Flow:
1. Boot Linux with KVM CPU (~seconds)
2. Switch to Timing CPU
3. Run TME benchmark
4. Dump stats and exit

### Without KVM (Atomic fallback):

```bash
# Modify config to use AtomicSimpleCPU for fast-forward
# (slower but works on macOS)
```

## x86 TSX

Requires the external TSX patch and O3 CPU:

```bash
# 1. Apply patch and build
./scripts/build.sh x86-tsx --patch /path/to/tsx.patch

# 2. Run
./scripts/run-x86-tsx.sh
```

Flow:
1. Boot Linux with KVM CPU (Linux host only)
2. Switch to O3 CPU (TSX requires O3)
3. Run RTM benchmark
4. Dump stats

## Cross-Platform Comparison

To compare all three implementations:

```bash
# Build everything
./scripts/build.sh all

# Run POWER8 HTM (any platform)
./scripts/run-power8-htm.sh

# Run ARM TME and x86 TSX (Linux host with KVM)
./scripts/run-arm-tme.sh
./scripts/run-x86-tsx.sh
```

Note: For a fair comparison, use the same memory system
(MESI_Three_Level_HTM Ruby protocol) and the same benchmark
logic (array sum in a transaction) for all three ISAs.
