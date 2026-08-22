# Build Guide

This document describes how to build gem5 for each transactional memory
implementation in this project.

## Prerequisites

### macOS

```bash
# Python 3.12+ (gem5 25.1 requires >=3.10; macOS 12 ships 3.9)
brew install python@3.12

# SCons build system
brew install scons

# Other dependencies
brew install pkg-config

# Cross-compiler for POWER8 workloads (via Lima VM)
brew install lima
limactl create --name=debian template://debian
limactl start debian
```

Inside the VM:
```bash
sudo apt update && sudo apt install -y gcc-powerpc64le-linux-gnu
```

### Linux

```bash
sudo apt install -y scons python3.12 python3.12-dev gcc-powerpc64le-linux-gnu
```

---

## Building gem5

### Quick Start

```bash
# Build gem5 with all ISAs and Ruby HTM protocols (takes 15-30 min)
./scripts/build.sh all

# Run the POWER8 HTM benchmark
./scripts/run-power8-htm.sh
```

### Unified Build Script

The `scripts/build.sh` script handles all build targets:

```
Usage: ./scripts/build.sh <target> [options]

Targets:
  all           Build gem5 for ALL ISAs (includes all three TM implementations)
  power         Build gem5 for POWER ISA only (POWER8 HTM)
  arm           Build gem5 for ARM ISA only (ARM TME)
  x86           Build gem5 for X86 ISA only
  x86-tsx       Build gem5 for X86 with TSX patch applied

Options:
  -j N          Parallel build jobs (default: host CPU count)
  --clean       Clean build
  --no-ruby     Build without Ruby support (faster, no HTM)
  --python X    Python config to use
  --debug       Build debug binary (gem5.debug)
  --patch FILE  Path to TSX patch file (for x86-tsx target)
```

### Build Examples

```bash
# 1. Build everything (best for development - includes all ISAs)
./scripts/build.sh all

# 2. Build only POWER (faster, smaller binary)
./scripts/build.sh power

# 3. Build ARM with TME
./scripts/build.sh arm

# 4. Build x86 with TSX patch
./scripts/build.sh x86-tsx --patch /path/to/tsx.patch

# 5. Clean build
./scripts/build.sh all --clean

# 6. Custom Python config
./scripts/build.sh all --python /opt/homebrew/bin/python3.12-config
```

### Python Configuration

gem5 25.1 requires Python >= 3.10. On macOS 12:

```bash
# Auto-detection (recommended)
./scripts/build.sh all

# Manual override
./scripts/build.sh all --python python3.12-config
```

The build script auto-detects `python3.12-config` on macOS (from
Homebrew's `python@3.12`). System Python 3.9 is insufficient.

---

## Running Simulations

### POWER8 HTM (bare-metal SE mode)

```bash
# 1. Build the benchmark binary (inside Lima VM)
limactl shell debian
cd /path/to/workloads/power8
make array_sum_bench

# 2. Run simulation
OUTDIR=./m5out/power8-htm ./scripts/run-power8-htm.sh
```

The POWER8 HTM implementation uses the `MESI_Three_Level_HTM` Ruby
protocol and the TimingSimpleCPU model.

### ARM TME (requires Linux host with KVM)

```bash
# On a Linux host with /dev/kvm:
./scripts/build.sh arm

# Prepare the config script
# (ARM TME requires KVM fast-forward → switch to Timing CPU)
vim configs/arm-tme-kvm.py
./scripts/run-arm-tme.sh
```

ARM TME supports:
- KVM fast-forward (Linux only) for booting Linux
- Atomic CPU fallback for SE mode
- Both classic caches and Ruby HTM protocol

### x86 TSX (requires external patch)

1. Obtain the TSX patch (see docs/x86-tsx-patch.md)
2. Build with the patch:

```bash
./scripts/build.sh x86-tsx --patch /path/to/tsx.patch
```

3. Run:

```bash
./scripts/run-x86-tsx.sh
```

The x86 TSX patch only supports the O3 CPU model. KVM fast-forward
requires a Linux host.

---

## Cross-Compiling Workloads

### POWER8 (using Lima VM)

```bash
# Build all POWER8 benchmarks
limactl shell debian
cd /path/to/workloads/power8
make clean && make
```

Available binaries:
| Binary | Type | Description |
|--------|------|-------------|
| `array_sum` | glibc-linked | Full array sum with printf. May hit unimplemented VSX/VMX in glibc startup. |
| `array_sum_bench` | bare-metal | Array sum using raw syscalls. No glibc dependency. Recommended. |
| `tbegin_tend` | bare-metal | Minimal test: `tbegin; beq; li; li; sc` |
| `htm_test` | bare-metal | Array sum with memory operations inside transaction. |

### ARM (native or cross-compile)

```bash
# Native on ARM host
gcc -O2 -mcpu=armv8.5-a -o array_sum array_sum.c

# Cross-compile on x86
aarch64-linux-gnu-gcc -O2 -mcpu=armv8.5-a -o array_sum array_sum.c
```

### x86 (native)

```bash
gcc -O2 -mrtm -o array_sum array_sum.c
```

---

## Simulator Binary Locations

After building, binaries are at:

| Target | Binary |
|--------|--------|
| all (ALL ISAs) | `gem5/build/ALL/gem5.opt` |
| POWER only | `gem5/build/POWER/gem5.opt` |
| ARM only | `gem5/build/ARM/gem5.opt` |
| x86 only | `gem5/build/X86/gem5.opt` |
| x86 with TSX | `gem5/build/X86_TSX/gem5.opt` |

---

## Build Troubleshooting

### "Can't find a working Python installation"

```bash
# Check Python version
python3 --version

# If < 3.10, use Homebrew Python
export PYTHON_CONFIG=python3.12-config
./scripts/build.sh all
```

### "RUBY_PROTOCOL_MESI_Three_Level_HTM not found"

The ALL build includes all Ruby protocols. If building a single-ISA target,
ensure `--with-ruby` is used (default with build.sh).

### Build times

| Target | Time | Size |
|--------|------|------|
| `all` | 15-30 min | ~160 MB |
| `power` | 8-15 min | ~80 MB |
| `arm` | 10-20 min | ~100 MB |
| `x86` | 10-20 min | ~100 MB |

(On M1 MacBook Pro with 10 jobs)

### "TSX patch does not apply cleanly"

The patch was created for gem5 ~2014 and needs rebasing for v25.1.
See docs/x86-tsx-patch.md for details.
