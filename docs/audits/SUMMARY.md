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
| **TSXSGL** | 3/5 | TSX capacity model, abort reason handling not captured | Medium |
| **TinySTM_WBCTL** | 4/5 | Spin-for-unlock loop, type merging not modeled | Low |
| **TinySTM_WBETL** | 4/5 | Lock retry logic, bitmap write-back not modeled | Low |
| **TinySTM_WT** | 4/5 | Write-through semantics match; incarnation counter modulo in model | Low |
| **PersistentSGL** | 3/5 | Deferred flush (TLA+) vs simultaneous dual-write (C++); mmap not modeled | Medium |

## Phase 2 Backends (PlusCal)

| Backend | Score | Key gaps | Risk |
|---------|-------|----------|------|
| **Romulus** | 4/5 | Spin-loop, read-validate re-check, fences abstracted (all documented) | Low |
| **TL2** | 3/5 | Validation lock-bit gap, guard-table aliasing, no fence tracking | Medium |
| **XTM** | 4/5 | Commit validation missing owner_tx_id check; fence annotations not added | Medium |
| **LEFTRIGHT** | 3/5 | No `lastFence`/fence annotations, no `endVersion`/extend model, no pre-lock validate, reversed read-clock ordering | Medium |
| **SwissTM** | 3/5 | Contention manager not modeled; no `endVersion`/extend model; no fence annotations | Medium |

## Phase 3 Backends (TLA+-only)

| Backend | Score | Key gaps | Risk |
|---------|-------|----------|------|
| **NOrec** | 3/5 | Plugin-mode bypass paths not modeled; no `lastFence` tracking; clock double-check abstracted | High |
| **DUDETM** | 1/5 | TLA+ model is high-level design sketch; actual impl is TinySTM WBCTL wrapper with forked replayer; no abort handling in model | High |
| **NVHTM** | 2/5 | No checkpoint/recovery in C++; no SGL fallback in C++ (RTM fail→pass-through); logging append vs dedup different | High |
| **SPHT** | 2/5 | `DurableValid` invariant FAILS; TSX retry model vs C++ no-retry; SGL PCL divergence; crash/recovery modeled but absent in C++ | High |
| **DistributedSGL** | 1/5 | TLA+ models client-server lock server; C++ impl is single-machine file-backed mmap spinlock — fundamentally different algorithms | High |
| **TiKV** | 3/5 | Unbounded counters prevent TLC termination; Percolator 2PC decomposed into 3 actions vs single `txn.commit()` in code | Medium |
| **TSXSim** | 3/5 | `TSXvsSGLSafety` invariant FAILS (SGL begin while TSX active); no `sgl_lock` in read-set tracking | Medium |
| **SimEngine** | 2/5 | Naming mismatch: `SimEngine.tla` models DES `engine.rs`, not `sim_engine.rs` replayer; cost mode, address translation absent | Medium |

## Bugs Found by TLC (Phase 5)

| Backend | Bug | Found by | Fixed in C++? |
|---------|-----|----------|---------------|
| **TinySTM_WBETL** | Write-conflict abort didn't release locks | Addr={0,1} model | ✅ (PlusCal fix documented) |
| **TinySTM_WBCTL** | NoConcurrentLocking too strict for multi-addr | Addr={0,1} model | ✅ (Relaxed to LockChain) |
| **TinySTM_WBETL** | Extend abort path skipped lock release | Addr={0,1} model (2026-06-23) | ✅ (PlusCal fix documented) |
| **SPHT** | `DurableValid` fails: read-only TX triggers GroupCommit with empty PCL | sequential model | ❌ (C++ lacks PCL guard for read-only TX) |
| **TSXSim** | `TSXvsSGLSafety` fails: SGL begin while TSX active (no `sgl_lock` in read-set) | 2-thread model | ❌ (TLA+ model bug; real HW prevents via cache-coherence) |
| **NVHTM** | `FreshLogOnBegin` fails: model checks active state instead of idle/transition | sequential model | ❌ (Model bug — invariant wording) |
| **NVHTM** | `CommitPhaseOrdering` fails: enters flush_log before checkpoint set | sequential model | ❌ (Model bug — violates own requirement) |

## Cross-Cutting Observations

1. **All backends** abstract away `isTMAddress()` checks — the TLA+ address space is always TM-tracked.
2. **All backends with lock retry** (WBCTL, WBETL) have significant spin-loop/extend abstractions.
3. **SGL** is the only 5/5 — its simplicity (lock+mem) leaves nothing to abstract.
4. **WT** scores highest (4/5) among the TinySTM variants because write-through maps naturally to TLA+'s direct memory model.
5. **Phase 3 backends span the full range (1-3/5):** DUDETM and DistributedSGL score 1/5 (model describes a completely different algorithm from C++), NVHTM and SPHT score 2/5 (major algorithmic gaps, TLC failures), NOrec/TiKV/TSXSim score 3/5 (core protocol captured with significant abstractior gaps).
6. **Hardware vs model fidelity:** TSXSim and NVHTM both demonstrate that hardware features (RTM, cache coherence, checkpoints) are the hardest to model faithfully — the abstraction layer either misses real HW guards (TSXSim SGL safety) or invents features with no C++ counterpart (NVHTM checkpoint).
7. **Distributed backends are the worst fit:** TiKV (3/5) and DistributedSGL (1/5) show that distributed consensus protocols (Percolator 2PC, lock server) are extremely hard to model in shared-memory TLA+ — the communication layer is either abstracted to omniscence or entirely misrepresents the implementation.

## Recommended Model Improvements

1. **TSXSGL**: Add `CacheLines` constant for capacity-bound read-set (approximate L1 cache sizing).
2. **PersistentSGL**: Remove the deferred flush phase; model write as simultaneous `mem[a]=v ∧ nvm[a]=v` to match C++ dual-write pattern.
3. **Add `lastFence` + `FenceFidelity` to remaining backends**: TSXSGL, TL2, XTM, LEFTRIGHT, SwissTM, Romulus.
4. **TLC heap increase for WT**: WT parallel model (with `lastFence`) requires >4GB heap — investigate TLC distributed mode or reduce fence granularity.
5. **Fix TSXSim SGL safety**: add `\A t2 \in Thread : mode[t2] # "tsx"` guard to `SGLBegin`.
6. **Fix SPHT DurableValid**: add read-only TX guard to prevent GroupCommit with empty PCL.
7. **Rename SimEngine.tla → DESEngine.tla** to reflect it models the DES engine, not the real-backend replayer.
