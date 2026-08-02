# GUST GPU Backend — Scalable Multi-Version Concurrency Control for GPUs

Single-pass CUDA kernel implementing **GUST** (Nunes, Castro, Romano —
IST / INESC-ID), a multi-version concurrency control scheme for GPUs.

## Algorithm summary

GUST is an MVCC protocol whose key contribution is replacing the
**CAS-based commit-log append** of classic MVCC (JVSTM, CSMV) with an
**AtomicINC**, which never fails and scales linearly under GPU
contention.

**Core structures**
- **VBox (Versioned Box)** — per-address circular array of the most
  recent committed `(timestamp, value)` pairs.  `head` is an atomic
  monotonic append counter; slot = `head % DEPTH`.  Version 0 is the
  "empty" sentinel; real versions are stored as `CTS + 1`.
- **GTS (Global Timestamp)** — logical clock counting *finalized* CL
  slots (committed or aborted).  Transactions snapshot it at begin →
  `startTS`.
- **CL (Commit Log)** — bounded circular buffer in global memory.  Each
  entry holds the transaction's state (free/pending/committed/aborted)
  and write-set.  A transaction reserves its slot (and thereby its
  **commit timestamp CTS**) via `AtomicINC` on a global `writePtr`.

**Transaction execution**
1. `TXBegin` snapshots GTS → `startTS`.
2. `TXRead` walks the VBox history for the newest version ≤ snapshot
   (read-own-writes first in a real deployment); records it in the
   read-set.
3. `TXWrite` buffers the new value in the private write-set.
4. `TXCommit` (update tx) runs four stages:
   - **(i) Pre-Validation** — intra-warp conflicts detected via
     `__ballot_sync`.  If a lower-numbered lane touches (reads or
     writes) an address this lane writes, the higher lane aborts early.
   - **(ii) CL Insertion** — warp leader `AtomicINC`s `writePtr` by
     `WARP_SIZE`, broadcasts the base; each lane gets `CTS = base +
     lane` and writes its state + write-set into `CL[CTS % CL_SIZE]`,
     followed by a `__threadfence()`.
   - **(iii) Validation** — hybrid **CCT + MRV**:
     * `CCT` (validation vs Concurrent Committed Transactions): scan
       `valPtr` from `CTS-1` downward while `valPtr ≥ GTS`.  These
       transactions may still be committing, so compare the read-set
       against each one's CL write-set; on any intersection, abort.
       Aborted slots are skipped.
     * `MRV` (Most Recent Version): once `valPtr < GTS` all earlier
       slots are finalized, so a single read-set scan suffices — abort
       if any read VBox holds a version newer than the snapshot.
   - **(iv) Write-Back** — append new versions to the VBoxes (fully
     parallel), then the warp leader waits until `GTS == base` and
     advances GTS by `WARP_SIZE`, atomically publishing the batch.
5. Read-only transactions skip validation entirely and never abort.

## Why AtomicINC instead of CAS

Extending the commit log via CAS serializes under massive GPU
parallelism: only one thread succeeds, all others spin and retry.  At
8960 threads on an RTX 6000 Ada, the paper measures **~100× more
concurrent AtomicINCs than CASes** (see `CAS_vs_AtomicINC.pdf` in the
paper).  GUST establishes the serialization order first (AtomicINC),
then validates against concurrently-committed transactions — CCT for
those still in flight, MRV once the GTS threshold guarantees all earlier
updates are visible.

## Fidelity notes vs the paper

- **Read-own-writes**: this single-pass test kernel builds its
  read-set before the write-set, so it does not exercise the write-set
  lookup; a real deployment must check the private write-set first.
- **CL wrap-around**: the paper aborts-and-retries a transaction whose
  target CL slot may still be needed for validation.  We implement a
  conservative guard (any non-free slot → abort).  With `CL_SIZE ≫
  num_warps × WARP_SIZE` the guard never fires in a single pass.
- **Values elided**: the kernel writes synthetic `lane + warp*32 + w`
  values; correctness of commit/abort decisions is governed purely by
  version metadata.
- **Batch publication spin**: the warp leader's `while (gts < base)`
  requires all warps co-resident (small grid).  Each prior batch
  finalizes exactly once, so the spin terminates.

## Layout

- `include/gpu_gust_api.h`     — constants, device structs, host API
- `cuda/gpu_gust_kernel.cuh`   — VBox/CL helpers + single-pass kernel
- `cuda/gpu_gust_host.cpp`     — host TM hooks (g++/hipcc compatible)
- `cuda/gpu_gust_runtime.cu`   — kernel launch wrapper (`<<< >>>`)
- `../../common/tm_gpu_platform.hpp` — CUDA/HIP portability layer

Build requires CUDA (`nvcc`) or HIP (`hipcc`); no GPU is available on
this development machine (macOS arm64), so the kernel is compile-time
reviewed only.

## Verification

A PlusCal/TLA+ model (`docs/proofs/GPU_GUST.tla`) captures the protocol
at warp granularity: warp-as-process, SIMT lockstep via shared `phase`,
per-thread read/write arrays, AtomicINC CL insertion, hybrid CCT+MRV
validation, and batch GTS publication.  Nine invariants include
`InvNoMissedConflict` (opacity — no committed tx may be followed by a
committed tx writing an address it read).

| Config | Scope | TLC result |
|--------|-------|------------|
| `GPU_GUST-small.cfg` | 2 warps × 2 threads × 2 addrs, `MaxCommits=1` | **1,887,633 states / 856,460 distinct, no errors** |
| `GPU_GUST.cfg` | `MaxCommits=2` | 25M+ distinct states, no violations before timeout |
| `GPU_GUST-liveness.cfg` | `Spec_WF` + `ProgressProp` (small) | **PASS** — no deadlock, no temporal violations |

The model's snapshot threshold (`startTS`, not `startTS+1`) exposed a
real semantic bug in an early kernel draft (see Fidelity notes); the
kernel was corrected to match the verified model.

## References

- Nunes, Castro, Romano. *GUST: Scalable multi-version concurrency
  control for GPUs*.  (Source paper; IEEE CAL 2014 follow-up work by the
  CSMV group.)
- JVSTM: Cachopo & Rito-Silva, 2006.  CSMV: Nunes, Castro, Romano,
  IPDPS 2022 / JPDC 2023.
