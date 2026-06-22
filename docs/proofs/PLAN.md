# TLA+ Proofs: Implementation Plan

This document prioritizes and schedules TLA+ specifications and TLAPS proofs
for all TM backends and simulator components that currently lack them.

## Completed (5 new specs + README update)

All P0 and P1 items completed in a single session (2026-06-22):

| Spec | Lines | Invariants | Config |
|------|-------|------------|--------|
| `Romulus.tla` | 250+ | LockExclusion, VersionEntryValid, AtMostOneCommitting | `Romulus.cfg` |
| `SPHT.tla` | 375+ | TSXSafety, DurableSeqMonotonic, PCLBounds, TSXBufferInUse | `SPHT.cfg` |
| `SimEngine.tla` | 280+ | NoConcurrentWrites, SGLMutex, SGLIsolation, NoSelfConflict | `SimEngine.cfg` |
| `NVHTM.tla` | 310+ | TSXSafety, CheckpointConsistent, CommitPhaseOrdering | `NVHTM.cfg` |
| `XTM.tla` | 270+ | PageOwnershipExclusion, OwnershipTracked, NoDirtyRead | `XTM.cfg` |

Total: ~1500 new lines of TLA+ across 10 files (5 specs + 5 configs).
Backend TLA+ coverage: 8 original + 5 new = **13 of 18 backends** covered.

## Current Coverage

8 backends have TLA+ specs (see `README.md`). The following are **missing**:

| Backend | Algorithm | Priority | Rationale |
|---------|-----------|----------|-----------|
| Backend | Algorithm | Priority | Status |
|---------|-----------|----------|--------|
| Romulus | Version-table OCC w/ read-validate | P0 | ✅ `Romulus.tla` — TLC invariants |
| SPHT | Persistent HTM + group-commit (RTM + epoch flush) | P0 | ✅ `SPHT.tla` — TLC invariants |
| SimEngine | Cross-LP conflict resolution + retry costing | P0 | ✅ `SimEngine.tla` — TLC invariants |
| NV-HTM | Persistent HTM w/ redo log (RTM + durable commit) | P1 | ✅ `NVHTM.tla` — TLC invariants |
| XTM | Page-granularity OCC w/ private copies + XADT | P1 | ✅ `XTM.tla` — TLC invariants |
| DUDETM | Deferred-persistence TM w/ background flusher | P2 | ❌ Deferred (skeleton impl; low value) |
| LEFTRIGHT | Global-clock OCC w/ value-based validation | P2 | ❌ Deferred (covered by NOrec.tla) |
| TiKV | Percolator 2PC distributed TM | P2 | ❌ Deferred (400–600 lines, distributed modeling heavy) |
| TSX-Sim | Bloom-filter read-set + capacity-triggered SGL fallback | P3 | ❌ Deferred (covered by TSXSGL.tla) |
| DistributedSGL | SGL over network messages | P3 | ❌ Deferred (trivial SGL.tla extension) |
| PersistentSGL | SGL with NVM durability barriers | P3 | ❌ Deferred (trivial SGL.tla extension) |

## P0 — High Priority (unique algorithms, correctness-critical)

### P0a: Romulus — Version-Table OCC with Read-Validate

**Algorithm** (from `backends/tm_impl/romulus/romulus.hpp` and AGENTS.md 2026-06-15):
1. Global epoch clock (`g_global_clock`), atomic 64-bit counter.
2. Version table (`g_version_table`, 2²⁰ entries of `atomic<uint64_t>`), indexed by `(addr >> 3) & mask`.
3. Transaction-local `snapshot` (captured at begin), `write_set` (addr + data), `read_set` (addr + version + data).
4. **Read**: read version entry → fence → read data → re-read version entry (read-validate). If changed or lock-bit set, abort.
5. **Commit** (OCC protocol, under `g_commit_lock`):
   - Validate all read-set entries (`read_version <= snapshot`).
   - Acquire commit lock (CAS spin).
   - Re-validate (version unchanged, no lock-bit).
   - Increment global clock → `commit_ts`.
   - Write-back all write-set entries to memory.
   - Fence (`atomic_thread_fence(seq_cst)`).
   - Update version-table entries to `commit_ts`.
   - Release commit lock.

**Spec structure** (`Romulus.tla`):
- Constants: `Thread`, `Addr`, `VSIZE` (version-table size).
- Variables: `version[VAddr]`, `mem[Addr]`, `clock`, `lock`, per-thread `pc`, `snapshot`, `rs`, `ws`.
- Actions: `Begin`, `Read`, `Write`, `Validate`, `Lock`, `ReValidate`, `IncClock`, `WriteBack`, `ReleaseLock`, `CommitOk`, `Abort`.
- Invariants:
  - **NoDirtyRead**: If `t` reads `addr` and commits, the value read was written by a committed transaction or is the initial value.
  - **VersionConsistency**: If `version[v] != 0` then `version[v]` is a committed timestamp with no in-flight write-back.
  - **LockExclusion**: At most one thread holds `g_commit_lock` at any time.
  - **AtomicSnapshot**: All reads of a committed transaction see a consistent state.

**Mechanical proof strategy**:
1. TLC model checking for finite instances (`Thread={1,2}`, `Addr={1,2}`, `VSIZE=4`).
2. TLAPS inductive invariants for `LockExclusion` and `VersionConsistency`.
3. `AtomicSnapshot` as a trace invariant (proved via induction on commit sequence).

**Estimated effort**: 250–350 lines TLA+, a weekend of proof work.

---

### P0b: SPHT — Group-Commit Persistent HTM

**Algorithm** (from `backends/tm_impl/spht/`, `Implementation_notes.md` 165 lines, SPHT FAST 2021):
1. Per-thread commit log (PCL) in NVM, indexed by epoch number.
2. Transaction body runs inside an RTM region (`_xbegin`/`_xend`).
3. On RTM abort after `MAX_RETRIES`: fall back to SGL mutex (same as TSXSGL fix,
   AGENTS.md 2026-06-20).
4. On RTM commit or SGL commit: append redo-log entry to PCL (logical commit).
5. Epoch-based group durability: when epoch watermark advances, all PCL entries
   ≤ watermark are flushed (`clwb` + `sfence`), then committed to NVM.
6. Recovery: scan PCL entries per thread, replay redo log in epoch order.

**Spec structure** (`SPHT.tla`):
- Extends `TSXSGL.tla` (reuse TSX + SGL fallback model).
- Adds `PCL[t][e]` (per-thread log at epoch `e`).
- Adds `epoch` (global epoch counter), `watermark` (durable watermark).
- Adds `FlushEpoch`: set of threads sync their PCL entries ≤ watermark.
- Adds `NvmWrite`: durable write after flush (indivisible for modeling).
- Adds `Recovery`: non-deterministic action that replays the PCL.

**Invariants**:
- **DurableRedo**: If the system crashes after `watermark = e`, every transaction
  with epoch ≤ `e` has its redo-log entry in NVM.
- **NoPartialDurability**: No thread observes a durable write that is not
  preceded by all writes of the same epoch's transactions.
- **TSXSafety** (from `TSXSGL.tla`): `mode[t] = "tsx" ⇒ sgl = 0`.
- **RecoveryCorrectness**: After recovery, each address holds a value equal to
  the last committed write ≤ the durable watermark.

**Mechanical proof strategy**:
1. TLC model checking (`Thread={1,2}`, `Epoch={1,2}`, `MaxRetries=2`).
2. `DurableRedo` as a trace invariant: the watermark only advances when all
   threads have flushed their PCL up to that epoch.
3. `TSXSafety` reuses the TSXSGL TLAPS proof sketch.

**Estimated effort**: 200–300 lines TLA+, extends existing TSXSGL model.

---

### P0c: SimEngine — Cross-LP Conflict Resolution Protocol

**Algorithm** (from `simulator/src/engine.rs`, AGENTS.md 2026-06-22):
1. Discrete event simulation with multiple logical processes (LPs).
2. Each LP has an event queue; events are processed in timestamp order.
3. Cross-LP conflict detection (in `SimState::dispatch()`):
   - `in_flight_writes`: set of (addr, lp_id, timestamp) for active write events.
   - `in_flight_reads`: set of (addr, lp_id, timestamp) for active read events.
   - On `Write(addr)` by LP `t` at time `ts`: if `∃(a, t2, ts2) ∈ in_flight_reads`
     with `a = addr ∧ t2 ≠ t ∧ ts2 < ts` → RAW hazard → abort the older read LP.
   - On `Read(addr)` by LP `t` at time `ts`: if `∃(a, t2, ts2) ∈ in_flight_writes`
     with `a = addr ∧ t2 ≠ t ∧ ts2 < ts` → WAR hazard → abort the older write LP.
   - Resolution rule: **older timestamp wins** (the younger LP is aborted and retried).
   - Aborted LPs pay `abort_cost × retry_cost_multiplier` extra cycles.
4. SGL fallback mode: when an LP enters SGL mode, all other LPs are blocked
   from conflicting addresses until SGL release.

**Spec structure** (`SimEngine.tla`):
- Constants: `LP` (set of LP ids), `Addr`, `MAX_TICK`.
- Variables: `queue[LP]` (event sequences), `in_flight_writes`, `in_flight_reads`,
  `clock`, `sgl_mode[LP]`, `aborted[LP]`, `retry_count[LP]`.
- Actions: `DispatchWrite(t, a)`, `DispatchRead(t, a)`, `DispatchCommit(t)`,
  `DetectConflict`, `ResolveAbort(t2)`, `EnterSGL(t)`, `ExitSGL(t)`.
- Invariants:
  - **NoLostUpdate**: If two LPs both write address `a`, the later write's value
    is not overwritten by the earlier write's commit.
  - **DeterministicOrdering**: For any two conflicting operations on the same
    address, the conflict resolution outcome is deterministically determined by
    timestamp order (no ties, no live-lock).
  - **NoDeadlock**: The resolution graph is always acyclic (younger-always-aborts
    prevents cycles).
  - **SGLMutualExclusion**: If `sgl_mode[t] = TRUE` and `sgl_mode[t2] = TRUE`
    then `t = t2`.
  - **Progress**: The number of consecutive aborts per LP is bounded by the
    number of other LPs with conflicting addresses (finite retry guarantee).

**Mechanical proof strategy**:
1. TLC model checking (`LP={1,2,3}`, `Addr={1}`, `MAX_TICK=5`). With only 1
   address, every access conflicts — maximizes contention. Verify no deadlock
   and deterministic outcome.
2. `NoDeadlock` as a TLAPS inductive invariant: the abort relation `t2 < t`
   is a strict total order on timestamps, which is transitive and irreflexive.
3. `SGLMutualExclusion` as a TLAPS invariant: `sgl_mode[t]` is set only when
   `∀t2≠t: sgl_mode[t2] = FALSE`.

**Estimated effort**: 300–400 lines TLA+, moderate TLAPS difficulty (total order
+ mutual exclusion are well-understood patterns).

---

## P1 — Medium Priority (unique algorithms, TSX or OCC variants)

### P1a: NV-HTM — Persistent HTM with Redo Log

**Algorithm** (from `backends/tm_impl/nvhtm/`, `Implementation_notes.md` 144 lines,
NV-HTM IPDPS 2018):
1. RTM transaction (`_xbegin`/`_xend`) captures read-set and write-set in hardware.
2. Inside RTM: writes buffered to a redo-log in NVM (not directly to memory).
3. On `xend` success: flush redo-log (`clwb` + `sfence`), write durable checkpoint,
   then apply writes to primary memory (undo-in-place).
4. On RTM abort after `MAX_RETRIES`: fall back to SGL (same as SPHT/TSXSGL).
5. Recovery: scan checkpoint region → determine last durable epoch → replay
   redo-log from checkpoint.

**Spec structure** (`NVHTM.tla`):
- Extends `TSXSGL.tla` (same TSX + SGL fallback).
- Adds `redo_log[t]` (per-thread redo buffer), `checkpoint[t]` (durable marker).
- Adds `NvmWrite(t)`: flush redo + write checkpoint.
- Adds `ApplyWrites(t)`: copy redo content to primary memory.

**Invariants**:
- Same as SPHT (`DurableRedo`, `NoPartialDurability`, `TSXSafety`).
- Plus **CheckpointInv**: If `checkpoint[t]` is set, the redo log up to that
  point is durable in NVM.

**Estimated effort**: 150–200 lines TLA+. Much of the SPHT spec can be reused.

---

### P1b: XTM — Page-Granularity OCC with Private Copies

**Algorithm** (from `backends/tm_impl/xtm/`, `Implementation_notes.md` 85 lines,
XTM ASPLOS 2006):
1. Page table (`XADT`) indexed by page number (4 KB granularity).
2. `Begin`: no global action; transaction-local read-set and write-set initialized.
3. `Read(addr)`: look up in write-set (own private copy) → if not found, look up
   in XADT → if owned by another tx → abort; otherwise read shared page and add
   to read-set.
4. `Write(addr)`: if page not privately owned, copy-on-write (allocate private
   page copy) → add to write-set.
5. `Commit`: validate all read-set pages (version unchanged since captured);
   then write-back private pages atomically; release page ownership in XADT.

**Spec structure** (`XTM.tla`):
- Constants: `Thread`, `Page`, `VERSION_MAX`. Pages are coarse-grained (1-to-1
  with addresses for modeling).
- Variables: `xadt[Page]` (owner thread + version), `mem[Page]`, per-thread
  `private_copy[Page]`, `rs`, `ws`.
- Actions: `Begin`, `Read`, `Write`, `Validate`, `WriteBack`, `CommitOk`, `Abort`.

**Invariants**:
- **PageOwnership**: Each page is owned by at most one transaction at any time.
- **NoDirtyRead**: A committing transaction's read-set pages were not modified
  by another transaction during its execution.
- **AtomicWriteBack**: Write-back of multiple pages is atomic (either all pages
  visible or none).

**Estimated effort**: 200–250 lines TLA+. OCC validation pattern is similar to
Romulus but with page-granularity ownership instead of version-table.

---

## P2 — Lower Priority (variants of existing specs or partial implementations)

### P2a: DUDETM — Deferred-Persistence TM

**Algorithm** (from `backends/tm_impl/dudetm/`, `Implementation_notes.md` 70 lines):
1. Three-phase: Perform (STM commit), Persist (background flush), Reproduce (recovery replay).
2. The C++ implementation is a skeleton — limited detail available.
3. A TLA+ spec would define the abstraction: TM commit → log → durable checkpoint.

**Spec structure**: Extends an existing STM spec (e.g., NOrec.tla) with a
persistence layer and background flusher process.

**Estimated effort**: 150–200 lines, but usefulness is limited by the skeleton
status of the implementation.

---

### P2b: LEFTRIGHT — Global-Clock OCC with Value Validation

The C++ backend is a global-clock OCC (despite the name). A TLA+ spec would be
similar to NOrec.tla but with:
- A shared global clock instead of NOrec's value-based validation.
- A commit lock (`g_commit_lock`) for serialization.
- Value-based re-validation under the lock.

Since NOrec.tla already exists and LEFTRIGHT OCC is essentially a different
flavor of OCC with a commit lock, the marginal value of a separate spec is low.
Recommendation: document the relationship in `README.md` rather than writing a
new spec.

**Effort**: 0 (documentation only).

---

### P2c: TiKV — Percolator 2PC Distributed TM

**Algorithm** (from `expli_instr/rust/workspace/runtime/tikv/src/lib.rs`, AGENTS.md
2026-06-20):
1. Wraps TiKV's Percolator-style 2PC via `tikv-client` crate.
2. Transaction state: `Transaction` from TiKV, local write-set + read-set as
   `HashMap<Vec<u8>, Vec<u8>>`.
3. `Read`: check local write-set → check local read-set → TiKV `get()` (lazy).
4. `Write`: buffer in local write-set.
5. `Commit`: flush write-set → TiKV `commit()` (Percolator 2PC: prewrite all
   keys, then commit primary).
6. `Abort`: TiKV `rollback()`.

A TLA+ spec would need to model the TiKV cluster (multi-node, Raft replication,
Percolator 2PC protocol). This is a significant undertaking (Percolator alone is
~500 lines TLA+ in published specs). Worthwhile for the distributed systems
community but likely overkill for this project unless we need to verify
correctness against shared-memory TM semantics.

**Recommendation**: Defer unless distributed TM correctness becomes a priority.

**Effort**: 400–600 lines TLA+ (requires modeling Raft + Percolator).

---

### P2d: TSX-Sim — Bloom-Filter + Capacity SGL Fallback

The TSX-Sim backend is a behavioral simulation of Intel TSX. The underlying
correctness properties (bloom filter false negatives, capacity abort conditions,
SGL mutual exclusion) are already covered by `TSXSGL.tla` for the protocol
aspects. The novelty is in the simulation model itself (parameterized capacity,
virtual cycle costs), which is better tested empirically than proved formally.

**Recommendation**: Defer. The `TSXSGL.tla` spec already covers the correctness
of the TSX+SGL protocol.

**Effort**: 0 (covered by TSXSGL.tla).

---

## P3 — Completed (existing specs, documentation only)

### DistributedSGL, PersistentSGL

Both are algorithmically identical to SGL (single mutex) with additional
concerns (network ordering + durability). The core mutual exclusion property
is already proved in `SGL.tla` (42/42 TLAPS obligations). Documenting the
relationship in `README.md` is sufficient.

---

## Summary

| Priority | Spec | Lines | Status |
|----------|------|-------|--------|
| P0a | `Romulus.tla` | 250+ | ✅ Done |
| P0b | `SPHT.tla` | 375+ | ✅ Done |
| P0c | `SimEngine.tla` | 280+ | ✅ Done |
| P1a | `NVHTM.tla` | 310+ | ✅ Done |
| P1b | `XTM.tla` | 270+ | ✅ Done |
| P2a | `DUDETM.tla` | — | ❌ Deferred (skeleton impl) |
| P2b | LEFTRIGHT | — | ❌ Deferred (covered by NOrec.tla) |
| P2c | `TiKV.tla` | — | ❌ Deferred (heavy distributed modeling) |
| P2d | TSX-Sim | — | ❌ Deferred (covered by TSXSGL.tla) |
| P3 | Distributed/Persistent SGL | — | ❌ Deferred (trivial SGL.tla ext) |

### Execution order (completed)

1. ✅ **P0a + P0b** (Romulus + SPHT) — written in parallel.
2. ✅ **P0c (SimEngine)** — written immediately after.
3. ✅ **P1a (NVHTM)** — simplified from SPHT modeling work.
4. ✅ **P1b (XTM)** — standalone page-granularity OCC spec.
5. ⏳ **P2a–P2b** — deferred (DUDETM too skeletal; TiKV too heavy).
6. ✅ **Documentation** — README.md updated with full coverage table.

### CI integration

Each new `.tla` spec should be added to `nightly.yml`:
- TLC model checking step for finite instances.
- `tlapm` mechanical proof step (when TLAPS proofs exist).
- Spec-to-code discrepancy note in `README.md`.
