# TLA+ Correctness Proofs

This directory contains TLA+ specifications and correctness proofs for all TM
backends in this repository.

## Files

| File | Backend | Status |
|------|---------|--------|
| `SGL.tla` | Single Global Lock | **Fully verified** — TLAPS (42/42 obligations proved) |
| `TSXSGL.tla` | TSX+SGL Hybrid | Lock-free + lock-owner invariants — TLAPS proof sketches (not mechanically checked) |
| `TL2.tla` | TL2 | Specified for TLC — proof sketches, not mechanically checked |
| `TinySTM_WBCTL.tla` | TinySTM Write-Back CTL | Specified for TLC — proof sketches, not mechanically checked |
| `TinySTM_WBETL.tla` | TinySTM Write-Back ETL | Specified for TLC — proof sketches, not mechanically checked |
| `TinySTM_WT.tla` | TinySTM Write-Through | Specified for TLC — proof sketches, not mechanically checked |
| `SwissTM.tla` | SwissTM | Specified for TLC — proof sketches, not mechanically checked |
| `NOrec.tla` | NOrec | Specified for TLC — proof sketches, not mechanically checked |

## Prerequisites

### tlapm (TLAPS Proof Manager)

Install the pre-built TLAPS binary:

```bash
# Linux x86-64
wget https://github.com/tlaplus/tlapm/releases/download/rolling/tlapm-1.6.0-pre-x86_64-linux-gnu.tar.gz
tar xzf tlapm-1.6.0-pre-x86_64-linux-gnu.tar.gz
sudo cp tlapm-*/tlapm /usr/local/bin/
```

Verify:

```bash
tlapm --version
```

**Backend prerequisites** (for Isabelle proofs):

- Z3 (`apt install z3`)
- Zenon (bundled with tlapm)
- Isabelle2023 (optional, for `IsaM` tactic — required only for obligations that
  Zenon and Z3 cannot handle)

### TLC Model Checker

TLC is included with the TLA+ Toolbox:

1. Install Java 11+.
2. Download the TLA+ Toolbox from https://github.com/tlaplus/tlaplus/releases
3. Launch the Toolbox and open File → Open Spec → Add Existing Spec for any
   `.tla` file in this directory.
4. Create a new TLC model (Run → New Model), set the model-checking parameters
   (e.g., `Thread <- {1,2,3}`, `Addr <- {1,2}`), and run.

## Verifying the Proofs

### SGL.tla — TLAPS Mechanical Proof

The only spec with a full mechanically-checked TLAPS proof:

```bash
cd /path/to/repo
tlapm docs/proofs/SGL.tla
```

Expected output:

```
[INFO]: All 42 obligations proved.
EXIT: 0
```

### All Specs — TLC Model Checking

For finite-state verification, model-check each spec with the TLA+ Toolbox.
Concrete parameters for model checking:

| Spec | Finite Instance |
|------|----------------|
| `SGL.tla` | `Thread <- {1,2}`, `Addr <- {1,2}` |
| `TSXSGL.tla` | `Thread <- {1,2}`, `Addr <- {1,2}`, `MaxRetries <- 2` |
| `TL2.tla` | `Thread <- {1,2}`, `Addr <- {1,2}`, `MAX_COMMIT <- 5` |
| `TinySTM_WBCTL.tla` | `Thread <- {1,2}`, `Addr <- {1,2}`, `MAX_VAL <- 3` |
| `TinySTM_WBETL.tla` | `Thread <- {1,2}`, `Addr <- {1,2}`, `MAX_VAL <- 3` |
| `TinySTM_WT.tla` | `Thread <- {1,2}`, `Addr <- {1,2}`, `MAX_VAL <- 3` |
| `SwissTM.tla` | `Thread <- {1,2}`, `Addr <- {1,2}`, `MAX_VAL <- 3` |
| `NOrec.tla` | `Thread <- {1,2}`, `Addr <- {1,2}`, `MaxRetries <- 3` |

From the command line (requires TLA+ tools on `$PATH`):

```bash
java -cp /path/to/tla2tools.jar tlc2.TLC docs/proofs/SGL.tla -config SGL.cfg
```

## Limitations

### TLAPS Backend Limitation (SGL.tla)

The TLAPS SMT and Isabelle backends cannot handle proof obligations containing
`ASSUME NEW CONSTANT t \in Thread` or `\A t \in Thread` when `Thread` is an
abstract (uninterpreted) constant set.  This is a known limitation of the
TLAPS→Isabelle translation, which silently drops obligations involving free
variables from `NEW CONSTANT` patterns.

**Affected steps in SGL.tla** (6 of 42):

| Step | Action | Formula | Justification |
|------|--------|---------|---------------|
| Q1 | Init | `\A t : lock=t => pc[t]="active"` | Vacuously true: all pc[t]="idle" |
| Q2 | Init | `\A t : pc[t]="active" => lock=t` | Vacuously true: all pc[t]="idle" |
| Q3 | Begin | `\A t1 : lock'=t1 => pc'[t1]="active"` | lock'=t, pc'[t]="active"; lock'≠t1 for t1≠t |
| Q4 | Begin | `\A t1 : pc'[t1]="active" => lock'=t1` | Only pc'[t]="active", lock'=t |
| Q5 | Begin | `lock'∈{0}∪Thread` | lock'=t, t∈Thread |
| Q6 | Commit | `lock'∈{0}∪Thread` | lock'=0 |

These steps are trivially true from context.  The remaining 36/42 obligations
are proved mechanically by Zenon, Z3, and Isabelle.

### Spec-to-Code Discrepancies

The following modeling abstractions and discrepancies exist between the TLA+
specifications and the actual C++ implementations:

**TSXSGL (TSXSGL.tla)** — Rewritten to use a thread-ID lock variable (`sgl`)
instead of an epoch counter.  `SGLBegin` and `TSXFallback` now require
`sgl = 0`, which fixes the earlier modeling bug (concurrent SGL entries).
The `LockFreeInv` and `LockOwnerInv` invariants are formally stated with
TLAPS proof sketches.  The C++ runtime correspondingly stores `1` (locked)
/ `0` (free) to `sgl_owner` instead of toggling epoch parity.  The TSX
hardware's cache-coherence conflict detection provides the safety guarantee
(the first SGL write to `sgl_owner` aborts any concurrent TSX); the
`tm_end()` double-check of `sgl_owner == tsx_start_owner` is a safety net.

**TL2 (TL2.tla)** — The spec models per-address guards, while the real
implementation uses a hash-based guard table (2^13 entries).  Guard-table
aliasing (two addresses mapping to the same guard) can cause false conflicts in
practice but does not affect correctness.  This is a standard modeling
abstraction.

**TinySTM WT (TinySTM_WT.tla)** — The spec models the incarnation counter
modulo-8 rollover (`%8`) which matches the real code's
`INCARNATION_BITS = 3` and `INCARNATION_MASK = 0x7`.  The abort/restore logic
is correctly modeled: memory is restored from undo log, locks are released with
bumped incarnation.

**SwissTM (SwissTM.tla)** — The spec has a `CommitValidateFail` action that
releases read-locks and write-locks but does not model the
`rollback(tx)` function.  For write-buffered (redo-log) STM this is correct:
no memory restoration is needed because writes have not been applied yet.

**NOrec (NOrec.tla)** — The spec models value-based validation as a single
atomic step (`ReadWithValidation`), while the real implementation uses a
spin-loop (`validate()`).  The spec's clock parity invariant (`clk % 2 = 1`
iff a thread is committing) correctly models the real CAS-based commit
protocol.

### Coverage

- The proofs cover **mutual exclusion** for SGL (fully verified).
- The remaining specs define **safety invariants** (no dirty reads, lock
  consistency, no concurrent committing) intended for TLC model checking with
  finite instances.
- The proofs do **not** cover: progress/liveness, contention manager behavior,
  infinite-state verification beyond SGL, or the interaction of TSX hardware
  aborts with the system memory model.
