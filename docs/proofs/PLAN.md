# TLA+ Proofs: PlusCal Migration Plan

## Status

All 18 backends have TLA+ specs (pure TLA+, action-based). TL2 has been converted to PlusCal as a proof of concept.

Goal: convert structurally-suitable backends to PlusCal for readability, while keeping the TLA+ generated code committed (since `pcal.trans` writes in-place).

## Phase 0: Build infrastructure

Add a `Makefile` to `docs/proofs/` that provides:
- `make check-<backend>` — run TLC on a single spec
- `make check` — run TLC on all specs
- `make tla` — run `pcal.trans` on all PlusCal specs (regenerates TLA+ in-place)
- `make clean` — remove `*.old`, `*_TTrace_*`, `*.bin`, `states/`
- `make check-all` — extended TLC with `-coverage 1`

Configurable via `TLA2TOOLS_JAR` (default: `/tmp/tla2tools.jar`).

## Phase 1: Simple backends → PlusCal

| Backend | Labels | PlusCal complexity | Effort |
|---------|--------|--------------------|--------|
| **SGL** | 3 (begin, active, end) | Trivial — `with` for read/write | 1 session |
| **TSXSGL** | 5 (begin, tsx_ok, tsx_abort, sgl, end) | TSX fallback in `either/or` | 1 session |
| **TinySTM_WBCTL** | 4 (begin, read, write, commit) | Write-back CTL | 1 session |
| **TinySTM_WBETL** | 4 (begin, read, write, commit) | Write-back ETL | 1 session |
| **TinySTM_WT** | 4 (begin, read, write, commit) | Write-through | 1 session |
| **PersistentSGL** | 4 (begin, read/write, persist, end) | SGL + durability label | 1 session |

## Phase 2: Medium-complexity backends → PlusCal

| Backend | Labels | Strategy |
|---------|--------|----------|
| **TL2** | DONE | Reference implementation |
| **Romulus** | 6 | Version-table OCC: start → read/write → validate → inc clock → write-back → release |
| **XTM** | 6 | Page-granularity OCC: start read/write → page copy → validate → write-back |
| **LEFTRIGHT** | 5 | Global-clock OCC: start → read/write → validate (value) → inc clock → release |
| **SwissTM** | 6 | Time-based + encounter locking: start (snapshot) → read (validate if stale) → write (lock) → commit |

## Phase 3: TLA+-only backends (retain existing)

These have complex concurrent structure that PlusCal's sequential-process model cannot express naturally:

| Backend | Reason |
|---------|--------|
| **NOrec** | CAS-based commit + value validation + retry counter. Already has TLAPS proof sketch. |
| **DUDETM** | 3-phase commit with concurrent flush. Phases interleave non-sequentially. |
| **NVHTM** | RTM conflicts + redo log + 2-phase abort. RTM semantics are non-deterministic. |
| **SPHT** | Group-commit + RTM + SGL fallback. Three interacting modes. |
| **DistributedSGL** | Lock-server messaging. Message arrival is non-deterministic. |
| **TiKV** | Percolator 2PC with per-key locks across network. |
| **TSXSim** | Simulation engine (bloom filter + capacity tracking). Not a TM algorithm. |
| **SimEngine** | Trace-driven simulation. Not a TM algorithm. |

## Phase 4: Cross-cutting additions (all specs)

After PlusCal conversion, append after `\* END TRANSLATION`:

1. **Fairness alternatives** (TLA+-only, beyond PlusCal's `-wf` flag):
   - `Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))`
   - `Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))`
   - `ProgressProperty == \A self \in Thread : (pc[self] = "L_active" ~> pc[self] = "L_idle")`

2. **Structural state restrictions** (limit state space for realistic checking):
   - `NoConcurrentCommit` — at most one thread in any committing state
   - `FairSchedule` — threads must attempt commit within N active steps

## Phase 5: Verification

For each backend:

| Check | Config | Expected |
|-------|--------|----------|
| Safety | Thread={1,2}, Addr={0,1}, MaxCommits=2 | All invariants pass |
| Sequential | Thread={1} only | All invariants pass (trivial) |
| Deadlock-free | `-deadlock` | No deadlock |
| Liveness | OPT-IN: `Spec_WF` + `ProgressProperty` | Property holds |

## Summary

| Phase | Scope | Effort |
|-------|-------|--------|
| 0 | Makefile | 1 session |
| 1 | 6 simple backends → PlusCal | 6 sessions |
| 2 | 4 medium backends → PlusCal | 8 sessions |
| 3 | 6 TLA+-only retentions (no-op) | 1 session |
| 4 | Fairness + invariants (all) | 2 sessions |
| 5 | Verification (all) | 3 sessions |
| **Total** | **11 new PlusCal specs** | **~21 sessions** |
