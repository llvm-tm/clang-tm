# gem5 TSX Calibration Guide

Calibrate gem5's `MESI_Three_Level_HTM` Ruby protocol to match real hardware
TSX performance. The calibration methodology uses calibrated Broadwell-EP cycle
costs from the `tsx_sim` DES backend as the reference.

## Overview

| Component | Value |
|-----------|-------|
| **Target hardware** | Intel Xeon E5-2648L v4 (Broadwell-EP stepping 1) @ 1.80 GHz |
| **L1 cache** | 32 KB, 8-way (4 cycle hit) |
| **L2 cache** | 256 KB, 8-way (12 cycle hit) |
| **L3 cache** | 2 MB/core slice (35 cycle hit) |
| **RTM costs (measured)** | `xbegin`: 60 cycles, `xend`: 178 cycles |
| **Benchmark** | Bank: 16 accounts, 500 random transfers |
| **Calibration target** | `<10% per-TX cycle error vs tsx_sim DES model` |

### How calibration works

Real hardware was profiled via RDTSC-instrumented TSX benchmarks
(`patches/profile/tsx/`). The measured cycle costs (XBEGIN=60, XEND=178, etc.)
are injected into gem5's HTM sequencer as configurable latencies and into
tsx_sim's cost model directly. The two simulators are then run with identical
workloads, and per-transaction cycle costs are compared.

---

## Prerequisites

### gem5 build dependencies

```bash
# Python 3.12+ (gem5 25.1)
brew install python@3.12 scons
```

### Cross-compilation for x86-64 Linux

```bash
# x86_64 assembler and linker (ELF support)
brew install x86_64-linux-gnu-binutils

# C cross-compiler
brew install x86_64-linux-gnu-gcc
# Fallback: use clang with -target x86_64-linux-gnu
```

### Rust toolchain (for tsx_sim)

```bash
# tsx_sim benchmarks need Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

---

## Building gem5 with x86 TSX Support

```bash
cd gem5_sim/gem5

# Build X86_TSX target (includes MESI_Three_Level_HTM Ruby protocol)
PYTHON_CONFIG=/opt/homebrew/opt/python@3.12/bin/python3.12-config \
scons build/X86_TSX/gem5.opt -j$(sysctl -n hw.ncpu)
```

On Linux:
```bash
scons build/X86_TSX/gem5.opt -j$(nproc)
```

---

## Building the Bank Benchmark

### Pre-requisite files

These files must exist in `/tmp/` before running:

- `/tmp/bank.c` — C bank benchmark with inline RTM asm
- `/tmp/crt0_sys.s` — minimal `_start` entry point
- `/tmp/sys.s` — raw Linux syscall wrappers (write, exit, nanosleep)

### Compile for gem5 (x86-64 Linux ELF)

```bash
# Compile C with clang cross-compilation (no libc needed)
clang -target x86_64-linux-gnu -ffreestanding -nostdlib -O2 \
  -c -o /tmp/bank.o /tmp/bank.c

# Assemble startup and syscall wrappers
x86_64-linux-gnu-as -o /tmp/crt0_sys.o /tmp/crt0_sys.s
x86_64-linux-gnu-as -o /tmp/sys.o /tmp/sys.s

# Link static ELF
x86_64-linux-gnu-ld -static -o /tmp/bank \
  /tmp/crt0_sys.o /tmp/bank.o /tmp/sys.o
```

Verify:
```bash
file /tmp/bank
# Output: ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked
```

---

## Running gem5 Simulation

### Configuration file: `/tmp/tsx_test_config.py`

Key settings in the config:
- **Clock**: 1.8 GHz (match Broadwell-EP)
- **CPU**: `X86TimingSimpleCPU`
- **Memory**: Ruby `MESI_Three_Level_HTM` protocol
- **Cache hierarchy**: L0=32KB, L1=256KB, L2=2MB
- **Sequencer**: `RubyHTMSequencer` with calibrated latencies

### Launch

```bash
cd gem5_sim/gem5

PYTHON_CONFIG=/opt/homebrew/opt/python@3.12/bin/python3.12-config \
  ./build/X86_TSX/gem5.opt \
  -d /tmp/m5out \
  /tmp/tsx_test_config.py \
  /tmp/bank
```

### Expected output

```
Initial total: 1600
Final total: 1600
Commits: 500
Aborts: 0
  capacity: 0  conflict: 0  explicit: 0  other: 0
PASS
Exiting @ tick 98895720 because exiting with last active thread context
```

### Key stats to extract

| Stat path | What it measures |
|-----------|-----------------|
| `system.cpu.numCycles` | Total CPU cycles simulated |
| `simInsts` | Total instructions simulated |
| `m_htm_transaction_cycles::mean` | Avg sequencer cycles per TX |
| `m_htm_transaction_instructions::mean` | Avg instructions per TX (from sequencer) |
| `htmTransCommitWriteSet::1` | TXs with write set size 1 |
| `htmTransCommitWriteSet::2` | TXs with write set size 2 |

```bash
# Quick stats dump
grep -E 'numCycles|simInsts|htm_trans|htmTrans' /tmp/m5out/stats.txt
```

---

## Running tsx_sim Simulation

### Build

```bash
cd gem5_sim/benchmarks/tsx_sim_bank
cargo build --release
```

### Launch

```bash
./target/release/tsx-sim-bank
```

### Expected output

```
Initial total: 1600
Final total: 1600
Commits: 500
Aborts: 0
PASS: Money conserved
  STATS (TSX SIM):
    Commits=500 Aborts=0 (rate=0.0%)
Expected cycles per transaction: 268
Expected total cycles (cost model): 134000
```

### Cost model breakdown

The tsx_sim cost model uses calibrated Broadwell-EP v4 measurements:

| Operation | Cycles | Count per TX | Subtotal |
|-----------|--------|-------------|----------|
| `XBEGIN` | 60 | 1 | 60 |
| L1 read hit | 5 | 2 | 10 |
| Bloom filter check | 2 | 4 (2 reads + 2 writes) | 8 |
| L1 write hit | 6 | 2 | 12 |
| `XEND` | 178 | 1 | 178 |
| **Total** | | | **268** |

---

## Comparison Script

### Usage

```bash
python3 gem5_sim/scripts/compare_gem5_tsxsim.py
```

### What it does

1. Runs gem5 simulation (parses `stats.txt`)
2. Runs tsx_sim simulation (parses stdout)
3. Computes per-transaction cycle cost for both
4. Reports absolute and relative error

### Expected output

```
=================================================================
  gem5 vs tsx_sim TSX Benchmark Comparison
  Target hardware: Broadwell-EP v4 (Xeon E5-2648L v4 @ 1.80 GHz)
=================================================================
...
  Per-transaction cycle cost:
    Gem5 sequencer cycles (A):      91.2
    Gem5 + XEND (A + 178):          269.2
    tsx_sim cost model:             268

  Error (gem5 sequencer adj - tsx_sim):
    Absolute: +1.2 cycles/TX
    Relative: +0.4%

  Verdict: EXCELLENT (<10% error)
```

### Interpretation

The primary error metric is:

```
error = (gem5_sequencer_cycles + XEND_latency) - tsx_sim_per_tx_cycles
        ───────────────────────────────────────────────────────────
                          tsx_sim_per_tx_cycles
```

This metric compares:
- **gem5**: sequencer cycles (HTM_START → HTM_COMMIT request, includes
  XBEGIN latency + body) + XEND committed latency
- **tsx_sim**: cost model prediction (XBEGIN + body + XEND)

An error of **<1 cycle per TX (0.4%)** indicates the calibration is
excellent for this single-threaded, no-conflict workload.

---

## Calibration Methodology

### Step 1: Identify target hardware

Find the machine profile in `tm_api_cpp/machine_profiles/`:

```bash
cat /path/to/tm_api_cpp/machine_profiles/broadwell_ep_v4.json
```

Key fields: CPU model, frequency, cache latencies, TSX cycle costs.

### Step 2: Map hardware parameters to gem5

| Real hardware | gem5 parameter | Default | Calibrated |
|---------------|----------------|---------|------------|
| 1.8 GHz clock | `clk_domain.clock` | `"2GHz"` | `"1.8GHz"` |
| L1 hit: 4 cycles | L0 `request_latency` + `response_latency` | 4 | 4 (unchanged) |
| L2 hit: 12 cycles | L1 latencies + internal links | ~8 | 8 (approx match) |
| L3 hit: 35 cycles | L2 + network + directory | ~15 | 15 (faster than real, OK for small working sets) |
| XBEGIN: 60 cycles | `htm_start_latency` | 0 | 60 |
| XEND: 178 cycles | `htm_commit_latency` | 0 | 178 |

### Step 3: How latencies are injected

The extra XBEGIN/XEND latencies are added in
`HTMSequencer::rubyHtmCallback()` (`src/mem/ruby/system/HTMSequencer.cc`):

```cpp
Tick extra = 0;
if (pkt->req->isHTMStart()) {
    extra = clockPeriod() * m_htmStartLatency;   // 60 cycles
} else if (pkt->req->isHTMCommit()) {
    extra = clockPeriod() * m_htmCommitLatency;  // 178 cycles
}
port->schedTimingResp(pkt, curTick() + extra);
```

This delays the response to the CPU, adding the calibrated cycle cost to
each XBEGIN and XEND instruction.

### Step 4: RubyHTMSequencer parameters

Defined in `src/mem/ruby/system/Sequencer.py`:

```python
class RubyHTMSequencer(RubySequencer):
    htm_start_latency = Param.Cycles(60, "Extra latency for XBEGIN")
    htm_commit_latency = Param.Cycles(178, "Extra latency for XEND")
```

Set in the config file:

```python
l0_cntrl.sequencer = RubyHTMSequencer(
    ...,
    htm_start_latency=60,
    htm_commit_latency=178,
)
```

### Step 5: Tuning other parameters

For workloads with larger working sets (cache misses), tune:

| Parameter | File | Effect |
|-----------|------|--------|
| `request_latency`, `response_latency` | L0/L1/L2 controllers | Cache hit latency |
| Network latencies | `SimpleNetwork` ext/int links | Interconnect delays |
| `directory_latency` | Directory controller | DRAM access time |
| Cache sizes | `RubyCache(size=...)` | Capacity abort threshold |

---

## Abort Handling (Multi-Threaded)

Fixed in `patches/030-htm-functionality.patch` (applies on top of in-tree `v25.1`):

| Scenario | gem5 (before) | gem5 (after) | tsx_sim |
|----------|---------------|--------------|---------|
| Explicit abort (XABORT) | `InvalidOpcode` (broken) | `GenericHtmFailureFault EXPLICIT` with `RAX=(code<<24)\|1` | Supported |
| Conflict abort (RW/WW) | `LD_FAIL` only, `ST` ignored (hid WW) | Both `LD_FAIL` + `ST_FAIL` → `MEMORY` fault `RAX=0x08\|0x02` (conflict+retry) | Supported |
| Capacity abort (32 lines) | Not implemented (512 lines) | `FAIL_SELF` → `SIZE` `RAX=0x10` (capacity, no retry) via `htmTxLines` counter in `L0cache.sm` | Supported |

Changes:
1. **`XBeginInst::completeAcc()`** (`src/arch/x86/insts/htmruby.cc:49`): `Mem!=0` now maps `HtmCacheFailure` → `GenericHtmFailureFault` (`SIZE/MEMORY/OTHER`) instead of `InvalidOpcode`; checkpoint restore sets `PC` to `rel` fallback and `faults.cc:115` sets `EAX` status bits (`EXPLICIT 0x01`, `RETRY 0x02`, `CONFLICT 0x08`, `CAPACITY 0x10`, `code<<24`).
2. **`XAbortInst::completeAcc()`**: passes `imm8` to `GenericHtmFailureFault` so `LOCK_BUSY 0xFF` vs `OWNER_CHANGED 0x01` are distinguishable for the `TSXSGL` spin loop (`TSXSGL_runtime.cpp:196`).
3. **`TimingSimpleCPU::completeDataAccess()`** (`src/cpu/simple/timing.cc:1030`): now faults on **both** `LD` and `ST` `htmTransactionFailedInCache()` and handles `FAIL_OTHER` → `OTHER`.
4. **`L0cache.sm`**: added `htmTxLines` counter (limit `32`) in `hars`/`haws` (`src/mem/ruby/protocol/MESI_Three_Level_HTM-L0cache.sm:65`); `hst`/`hat`/`hct` reset it. Eviction of transactional line still aborts via `hfts`/`hftm`. multi-threaded `X86TimingSimpleCPU` with Ruby `MESI_Three_Level_HTM` is already supported; `O3` needs `LSQ` `ST` path as well (not yet patched, use `timing` for HTM).

---

## File Reference

| File | Purpose |
|------|---------|
| `gem5/src/arch/x86/insts/htm.hh` | TSX instruction class declarations |
| `gem5/src/arch/x86/insts/htm.cc` | Common implementations |
| `gem5/src/arch/x86/insts/htmruby.cc` | Ruby-path implementations (initiateAcc/completeAcc) |
| `gem5/src/mem/ruby/system/HTMSequencer.hh` | Sequencer header with latency members |
| `gem5/src/mem/ruby/system/HTMSequencer.cc` | Sequencer with calibrated delay injection |
| `gem5/src/mem/ruby/system/Sequencer.py` | Python param definitions |
| `/tmp/tsx_test_config.py` | gem5 SE config with MESI_Three_Level_HTM |
| `/tmp/bank.c` | C bank benchmark (inline asm RTM) |
| `simulator/` | Rust bank benchmark for tsx_sim |
| `gem5_sim/scripts/compare_gem5_tsxsim.py` | Comparison script |
| `tm_api_cpp/machine_profiles/broadwell_ep_v4.json` | Hardware profile |
| `tm_api_cpp/explicit_api/rust/workspace/runtime/tsx_sim/src/lib.rs` | Rust DES backend with cycle costs |

---

## Troubleshooting

### "Can't find a working Python installation"

```bash
export PYTHON_CONFIG=/opt/homebrew/opt/python@3.12/bin/python3.12-config
export PATH="/opt/homebrew/opt/python@3.12/bin:$PATH"
scons build/X86_TSX/gem5.opt ...
```

### "RUBY_PROTOCOL_MESI_Three_Level_HTM not found"

Ensure the `X86_TSX` build target is used (not `X86`).
The `X86` target does not include the HTM Ruby protocol.

### gem5 hangs during simulation

For 500+ transactions with Ruby's detailed cache model, the simulation
may take several minutes of wall time. The expected tick rate is:
- ~260K insts/s wall time on Apple M1 (for this config)
- ~90M ticks for 500 transactions

If simulation appears hung:
1. Check `ps aux | grep gem5` — the process should be consuming CPU
2. Reduce `NUM_TRANSFERS` to 50 in `bank.c` for faster iteration
3. Try with `--debug-flags=HtmMem` to see HTM sequencer activity

### tsx_sim build fails

Ensure the `runtime-tsx-sim` dependency path in `Cargo.toml` is correct:

```toml
[dependencies]
runtime-tsx-sim = { path = "../../explicit_api/rust/workspace/runtime/tsx_sim", features = ["simulation", "stats"] }
```

The path is relative from `simulator/`.

### Address boundary errors

The bank benchmark's `accounts` array uses heap allocation (`Vec<u64>`).
Heap addresses vary between runs but are stable within a run. The tsx_sim
cost model counts cache-line-aligned accesses, which is independent of
absolute address values.
