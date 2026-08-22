# POWER8 HTM: Architecture & gem5 Implementation Plan

## Overview

IBM POWER8 (Power ISA v2.07) introduced Hardware Transactional Memory (HTM).
It was also present in POWER9 but **removed in Power10** (see ISA v3.1 Appendix A).

Unlike x86 TSX (cache-line-granularity, L1-based) and ARM TME (L1-based with
MESI_Three_Level_HTM), POWER8 HTM uses **L2 cache for write buffering** with a
**Content-Addressable Memory (L2TMCAM)** for conflict detection.

## POWER8 HTM Architecture

### Instructions

| Instruction | Description |
|---|---|
| `tbegin.` | Begin a transaction; CR0 set on abort |
| `tend.` | Commit a transaction |
| `tabort.` | Explicitly abort a transaction |
| `tsr` | Transaction Suspend/Resume |
| `tcheck` | Test if transaction is active |

### Key Features

1. **Rollback-Only Transactions (ROT)**
   - Writes are tracked and applied atomically
   - Reads are NOT tracked (no read-set overhead)
   - Enables large write transactions without read-set capacity limits
   - Conflicts detected only on write-write pairs

2. **Suspend/Resume**
   - `tsr` suspends an active transaction
   - Non-transactional code (syscalls, I/O) can run during suspension
   - `tsr` with resume flag re-activates the transaction
   - Enables transactions that would otherwise abort on interrupts

3. **Survives Interrupts**
   - Transactions can survive context switches and interrupts
   - Checkpointed register state is saved/restored by OS
   - Linux kernel support: `PPC_TRANSACTIONAL_MEM`

4. **Checkpointed Registers**
   - All GPRs, FPRs, VRs/VSRs, LR, CTR, CR, FPCSR, and status flags

5. **Abort Causes**
   - Cache line conflicts (read-write or write-write)
   - L2 cache capacity overflow
   - Explicit `tabort.`
   - Signals, context switches, system calls (unless suspended)
   - See Power ISA v2.07 Book I, Chapter 8

### Conflict Detection

POWER8 uses L2 cache for conflict detection with a **L2TMCAM**
(content-addressable memory). The L2 cache holds pre-transactional values
for versioning (similar to ARM TME's L2 role, but at L2 not L1).

## Integration with gem5's Generic HTM Framework

gem5 already has a generic HTM framework at `src/arch/generic/htm.hh`
that provides:
- `BaseHTMCheckpoint` for register save/restore
- `HtmFailureFault` for abort handling
- `HtmCmd` requests for start/commit/abort
- Integration with `MESI_Three_Level_HTM` Ruby protocol

### Implementation Plan

#### Phase 1: ISA Support (src/arch/power/)

| File | Description |
|---|---|
| `htm.hh` / `htm.cc` | POWER-specific `HTMCheckpoint`: save/restore GPRs, FPRs, VRs, LR, CTR, CR |
| `insts/tm.hh` / `tm.cc` | Instruction classes for `tbegin`, `tend`, `tabort`, `tsr`, `tcheck` |
| `isa/htm.isa` | Decode tree entries mapping opcodes to TM instructions |
| `isa/main.isa` | Include `htm.isa` in the Power ISA decoder |

Key points for `HTMCheckpoint`:
- Save all GPRs (r0-r31), FPRs (f0-f31), VRs (v0-v31 if VSX enabled)
- Save LR, CTR, CR (condition register)
- On restore: `HtmFailureFault` restores checkpoint, sends abort to cache

#### Phase 2: Memory Model (Ruby Protocol)

POWER8 HTM uses the existing **MESI_Three_Level_HTM** protocol since:
- L0 (L1 in stdlib) marks transactional read/write sets
- L1 (L2 in stdlib) holds pre-transactional values for versioning
- Conflict detection on invalidation to read/write set lines
- For POWER-specific ROT behavior: add `HTM_ROT` request type that
  skips read-set tracking

Alternatively, create `MESI_Three_Level_HTM_ROT` variant.

#### Phase 3: CPU Model Support

| CPU Model | Status |
|---|---|
| `AtomicSimpleCPU` | Supported via generic HTM framework |
| `TimingSimpleCPU` | Supported |
| `O3CPU` | Needs frontend/commit stage tracking (like ARM TME) |

Modification needed in `src/cpu/`:
- `SimpleThread`: holds `HTMCheckpoint` instance (already generic)
- `O3` frontend/commit: track transaction depth (pattern from `iew_impl.hh`)

#### Phase 4: SE Mode Support

Add HTM syscall emulation for `tbegin`/`tend`/etc. This allows running
POWER8 HTM benchmarks without full-system OS boot:

```python
# In src/arch/power/linux/se_workload.cc or similar
```

## References

1. [POWER8 TM: Transactional memory support in the IBM POWER8 processor - IBM J. Res. Dev 2015](https://research.ibm.com/publications/transactional-memory-support-in-the-ibm-power8-processor)
2. [Robust architectural support for transactional memory in the power architecture - ISCA 2013](https://dl.acm.org/doi/10.1145/2508148.2485942)
3. [Power ISA v2.07 - Transactional Memory chapter](https://fileadmin.cs.lth.se/cs/education/EDAN25/PowerISA_V2.07_PUBLIC.pdf)
4. [Linux kernel POWER8 TM support](https://docs.kernel.org/arch/powerpc/transactional_memory.html)
5. [GCC POWER8 HTM builtins](https://gcc.gnu.org/onlinedocs/gcc-15.1.0/gcc/PowerPC-Hardware-Transactional-Memory-Built-in-Functions.html)
6. [power-gem5 (full-system POWER in gem5)](https://github.com/power-gem5/gem5-support-package)
7. [P8TM: POWER8 transactional memory framework](https://github.com/shadyalaa/POWER8TM)
8. [gem5 generic HTM infrastructure](https://doxygen.gem5.org/release/v21-0-1-0/arch_2generic_2htm_8hh_source.html)
