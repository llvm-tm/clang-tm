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
| **TL2** | (not yet audited) | | |
| **XTM** | (not yet audited) | | |
| **LEFTRIGHT** | (not yet audited) | | |
| **SwissTM** | (not yet audited) | | |

## Phase 3 Backends (TLA+-only)

| Backend | Score | Key gaps | Risk |
|---------|-------|----------|------|
| **NOrec** | (not yet audited) | | |
| **DUDETM** | (not yet audited) | | |
| **NVHTM** | (not yet audited) | | |
| **SPHT** | (not yet audited) | | |
| **DistributedSGL** | (not yet audited) | | |
| **TiKV** | (not yet audited) | | |
| **TSXSim** | (not yet audited) | | |
| **SimEngine** | (not yet audited) | | |

## Bugs Found by TLC (Phase 5)

| Backend | Bug | Found by | Fixed in C++? |
|---------|-----|----------|---------------|
| **TinySTM_WBETL** | Write-conflict abort didn't release locks | Addr={0,1} model | ✅ (PlusCal fix documented) |
| **TinySTM_WBCTL** | NoConcurrentLocking too strict for multi-addr | Addr={0,1} model | ✅ (Relaxed to LockChain) |
| **TinySTM_WBETL** | Extend abort path skipped lock release | Addr={0,1} model (2026-06-23) | ✅ (PlusCal fix documented) |

## Cross-Cutting Observations

1. **All backends** abstract away `isTMAddress()` checks — the TLA+ address space is always TM-tracked.
2. **All backends with lock retry** (WBCTL, WBETL) have significant spin-loop/extend abstractions.
3. **SGL** is the only 5/5 — its simplicity (lock+mem) leaves nothing to abstract.
4. **WT** scores highest (4/5) among the TinySTM variants because write-through maps naturally to TLA+'s direct memory model.
5. **TSXSGL** and **PersistentSGL** score 3/5 due to hardware/storage abstractions inherent to their domains.

## Recommended Model Improvements

1. **TSXSGL**: Add `CacheLines` constant for capacity-bound read-set (approximate L1 cache sizing).
2. **PersistentSGL**: Remove the deferred flush phase; model write as simultaneous `mem[a]=v ∧ nvm[a]=v` to match C++ dual-write pattern.
3. **Add `lastFence` + `FenceFidelity` to remaining backends**: TSXSGL, TL2, XTM, LEFTRIGHT, SwissTM, Romulus.
4. **TLC heap increase for WT**: WT parallel model (with `lastFence`) requires >4GB heap — investigate TLC distributed mode or reduce fence granularity.
