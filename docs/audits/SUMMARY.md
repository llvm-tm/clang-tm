# Audit Summary: TLA+ Specs vs C++ Implementations

## Methodology

Each backend audited per [AUDIT_PLAN.md](../proofs/AUDIT_PLAN.md) 4-step process:
1. Semantic abstraction identification
2. Per-backend abstraction gaps
3. Cross-validation (analysis phase; trace validation TBD)
4. Invariant cross-check

## Score Guide

| Score | Meaning |
|-------|---------|
| **5/5** | Perfect match — no abstraction gap |
| **4/5** | Minor deviations (fences, spin loops, read-validate re-check) |
| **3/5** | Significant protocol abstractions (type merging, lock retry, TSX capacity) |
| **2/5** | Major algorithmic differences (would require separate spec to capture) |
| **1/5** | Backend does what the spec describes but not vice versa |

## Phase 1 Backends (PlusCal)

| Backend | Score | Key gaps | Risk |
|---------|-------|----------|------|
| **SGL** | 5/5 | None — lock+mem is the entire impl (read/write/version are proof scaffolding) | None |
| **TSXSGL** | 4/5 | TSX capacity model, abort reason handling not captured; fences added | Low |
| **TinySTM_WBCTL** | 4/5 | Spin-for-unlock loop, type merging not modeled | Low |
| **TinySTM_WBETL** | 4/5 | Lock retry logic, bitmap write-back not modeled | Low |
| **TinySTM_WT** | 4/5 | Write-through semantics match; incarnation counter modulo in model | Low |
| **PersistentSGL** | 3/5 | Deferred flush (TLA+) vs simultaneous dual-write (C++); mmap not modeled | Medium |

## Phase 2 Backends (PlusCal)

| Backend | Score | Key gaps | Risk |
|---------|-------|----------|------|
| **Romulus** | 4/5 | Spin-loop, read-validate re-check abstracted; fences added ✅ | Low |
| **TL2** | 4/5 | Validation lock-bit gap, guard-table aliasing (core design feature); fences added | Low |
| **XTM** | 4/5 | Commit validation missing owner_tx_id check (C++ gap); fences added ✅ | Low |
| **LEFTRIGHT** | 4/5 | No `endVersion`/extend model, no pre-lock validate, reversed read-clock ordering; fences added | Low |
| **SwissTM** | 4/5 | Contention manager not modeled; no `endVersion`/extend model; fences added | Low |

## Phase 3 Backends (TLA+-only)

| Backend | Score | Key gaps | Risk |
|---------|-------|----------|------|
| **NOrec** | 3/5 | Plugin-mode bypass paths not modeled; no `lastFence` tracking; clock double-check abstracted | High |
| **DUDETM** | 1/5 | TLA+ model is high-level design sketch; actual impl is TinySTM WBCTL wrapper with forked replayer; no abort handling in model | High |
 | **NVHTM** | 3/5 | `FreshLogOnBegin` removed (state invariant impossible: log fills during TX); `CommitPhaseOrdering` fixed; TSX retry + SGL begin now check `tsx_mode`; `Spec_WF` added | Medium |
| **SPHT** | 3/5 | `DurableValid` removed (invalid invariant: read-only TXs don't add PCL entries); TSX retry now checks `sgl=0`; `Spec_WF` + `Completion` added | Medium |
| **DistributedSGL** | 1/5 | TLA+ models client-server lock server; C++ impl is single-machine file-backed mmap spinlock — fundamentally different algorithms | High |
| **TiKV** | 3/5 | Unbounded counters cause large state space; Percolator 2PC decomposed vs single `txn.commit()` in code | Medium |
| **TSXSim** | 3/5 | `TSXvsSGLSafety` replaced with `NoSGLTSXOverlap` (hardware-enforced guard); `Spec_WF` + `TransactionProgress` added | Medium |
| **SimEngine** | 3/5 | Naming mismatch: models DES engine not replayer; all invariants now hold with WAW conflict detection + SGL quiesce | Medium |

## Bugs Found by TLC (Phase 5)

| Backend | Bug | Found by | Status |
|---------|-----|----------|--------|
| **TinySTM_WBETL** | Write-conflict abort didn't release locks | Addr={0,1} model | ✅ Fixed in PlusCal |
| **TinySTM_WBCTL** | NoConcurrentLocking too strict for multi-addr | Addr={0,1} model | ✅ Relaxed to LockChain |
| **TinySTM_WBETL** | Extend abort path skipped lock release | Addr={0,1} model (2026-06-23) | ✅ Fixed in PlusCal |
| **NVHTM** | `FreshLogOnBegin` impossible as state invariant | sequential model | ✅ Removed (transition property) |
| **NVHTM** | `CommitPhaseOrdering` too strict for flush_log/write_cp | sequential model | ✅ Fixed invariant wording |
| **NVHTM** | `TSXRetryOrFallback` ELSE didn't clear tsx_mode/redo_log | 2-thread model | ✅ Fixed — clears tsx_mode, checks sgl=0 |
| **NVHTM** | `SGLBegin` didn't check `tsx_mode[other]` | 2-thread model | ✅ Added guard |
| **SPHT** | `DurableValid` not a valid invariant (read-only TX vs PCL length) | sequential model | ✅ Removed |
| **SPHT** | `TSXRetryOrFallback` ELSE didn't set `tsx_mode=FALSE` | 2-thread model | ✅ Fixed — sets tsx_mode, checks sgl=0 |
| **SPHT** | `SGLBegin` didn't check `tsx_mode[other]` | 2-thread model | ✅ Added guard |
| **TSXSim** | `TSXvsSGLSafety` too strong (coexisting TSX+SGL across threads is valid) | 2-thread model | ✅ Replaced with `NoSGLTSXOverlap` |
| **TSXSim** | `SGLBegin`/`TSXFallback` didn't check other TSX threads | 2-thread model | ✅ Added tsx_mode guard |
| **SimEngine** | `in_flight_writes`/`in_flight_reads` missing from UNCHANGED in ELSE branches | TLC parsing error | ✅ Fixed |
| **SimEngine** | `NoSelfConflict` invalid (same-LP read+write is valid) | 2-LP model | ✅ Removed |
| **SimEngine** | `WriteAddr` didn't detect WAW conflicts | 2-LP model | ✅ Added `conflicting_writers` check |
| **SimEngine** | `EnterSGL` didn't quiesce other LPs (new TX could start after SGL entry) | 2-LP model | ✅ EnterSGL checks `in_tx[other]=FALSE`; BeginTx checks `sgl_mode[none]` |
| **SimEngine** | `ExitSGL` left stale in-flight ops | 2-LP model | ✅ ExitSGL clears all in-flight tracking |
| **DistributedSGL** | `AtMostOnePending` too strict (two concurrent lock requests are valid) | 2-client model | ✅ Removed from cfg

## Cross-Cutting Observations

1. **All backends** abstract away `isTMAddress()` checks — the TLA+ address space is always TM-tracked.
2. **All backends with lock retry** (WBCTL, WBETL) have significant spin-loop/extend abstractions.
3. **SGL** remains the only 5/5 (lock+mem is trivially captured). **TSXSGL**, **Romulus**, **XTM**, **TL2**, **LEFTRIGHT**, **SwissTM** all upgraded to 4/5 after adding fence annotations.
4. **Fence annotations (`lastFence`+`FenceFidelity`) now added to 9 backends**: TinySTM_WBCTL, WBETL, WT, TSXSGL, TL2, XTM, LEFTRIGHT, SwissTM, Romulus.
5. **Phase 3 backends span the full range (1-3/5):** DUDETM and DistributedSGL score 1/5 (model describes a completely different algorithm from C++), NVHTM and SPHT upgraded to 3/5 (TLC invariants fixed, hardware guards added), NOrec/TiKV/TSXSim/SimEngine score 3/5 (core protocol captured with significant abstraction gaps).
6. **Hardware vs model fidelity:** TSXSim and NVHTM both demonstrate that hardware features (RTM, cache coherence, checkpoints) are the hardest to model faithfully.
7. **Distributed backends are the worst fit:** TiKV (3/5) and DistributedSGL (1/5) show that distributed consensus protocols are extremely hard to model in shared-memory TLA+.
8. **Fence annotations are coarse:** `lastFence[t]` records a single tag (`"sc"`/`"acq"`/`"rel"`) per label but cannot distinguish `atomic_signal_fence` (compiler barrier) from `atomic_thread_fence` (CPU fence) or bundled RMW+ordering (`fetch_add(acq_rel)`). `FenceFidelity` only checks `writeSet ≠ {} ⇒ a fence happened somewhere` — no guarantee of sufficient strength or correct placement. A proper memory-model proof would require something like `CAT`/`herd7`. Present annotations are a documentation/consistency cross-check, not a formal memory-model verification.

## Recommended Model Improvements

1. **PersistentSGL**: Remove the deferred flush phase; model write as simultaneous `mem[a]=v ∧ nvm[a]=v` to match C++ dual-write pattern.
2. **TLC heap increase for WT**: WT parallel model (with `lastFence`) requires >4GB heap — investigate TLC distributed mode or reduce fence granularity.
3. **Liveness check**: TLC has never been run with `Spec_WF` on any backend — all checks use `-deadlock` only. Add a `make liveness` target.
4. **TiKV timeouts**: TiKV's unbounded counters (committed[t], tx_seq[t]) generate large state spaces. Add a `MaxTx=2` bound for TLC.
5. **Rename SimEngine.tla → DESEngine.tla** to reflect it models the DES engine, not the real-backend replayer.
