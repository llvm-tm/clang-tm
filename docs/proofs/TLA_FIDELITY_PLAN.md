# TLA+ Fidelity Plan

Gaps found during 2026-07-02 implementation review. The old `FIDELITY_IMPROVEMENT_PLAN.md` (phases 1–7) is complete; this replaces it with remaining TLA+ spec gaps only.

---

## Priority order

### P1 — Spec omissions that mask real bugs

| # | Backend | Gap | TLA+ fix | Effort |
|---|---------|-----|----------|--------|
| 1 | TinySTM (WBCTL, WBETL, WT) | `L_validate` checks version only; misses lock-held-by-other-thread. C++ correctly checks both. Spec is unfaithful — TLC cannot verify actual safety. | Add `(lock[a][1] = 0 \/ lock[a][2] = self)` to validate predicate | 3 lines |
| 2 | SwissTM | `r_lock`/`r_ver` modeled as separate fields but C++ uses single combined field with `READ_LOCKED` sentinel. Check-then-set modeled as atomic, C++ uses per-OREC exchange. | Replace `OREC_RLOCK`/`OREC_RVER` with single `orec` word; model exchange semantics | 50 lines |

### P2 — Algorithmic paths not captured

| # | Backend | Gap | TLA+ fix | Effort |
|---|---------|------|----------|--------|
| 3 | SwissTM | `extend()` / `validate()` not modeled. C++ does read-time validation and early abort; spec assumes reads always succeed. | Add `L_extend` label between `L_active` reads | 40 lines |
| 4 | SwissTM | False sharing (`LOCK_EXTENT=4`): spec assumes bijection between Addr and ORECs. C++ writes to two addresses sharing an OREC only acquire lock once. | Add `owned_orecs` tracking; map Addr → ORec via `a >> 4` | 30 lines |
| 5 | TL2 | Spec locks all guards atomically; C++ acquires one at a time with partial-lock intermediate states. | Model sequential lock acquisition with sorted addresses | 30 lines |
| 6 | TSXSGL | TSX capacity abort not modeled. Real HW falls back to SGL when L1 cache tracking is exhausted. | Add `capacity[t]` variable; non-deterministic fallback | 25 lines |

### P3 — Cross-cutting model abstractions

| # | Backend | Gap | TLA+ fix | Effort |
|---|---------|------|----------|--------|
| 7 | All | Queue-mode path not modeled. LEFTRIGHT, Romulus, XTM have queue-mode execution that skips all read-set tracking. | Add `queueMode` variable; alternative short-circuit commit path | 60 lines |
| 8 | All | Address classification (stack/global/region) not modeled. The `isTMGlobal` bypass was invisible to TLC and existed in every backend. | Add `AddressSpace` type (TMRegion / TMGlobal / Stack) and per-address tag | 40 lines |

---

## Effort summary

| Priority | Items | Lines | Score impact |
|----------|-------|-------|-------------|
| P1 | 2 | ~53 | +1–2 (closes known fidelity gaps) |
| P2 | 4 | ~125 | +0.5–1 each (adds coverage) |
| P3 | 2 | ~100 | +0 (prevents regression) |
| **Total** | **8** | **~278** | |

Models unaffected by these gaps (correct as-is): SGL, PersistentSGL, TSXSGL (core protocol), NOrec (basic protocol), DUDETM, DistributedSGL, TiKV (design sketches).
