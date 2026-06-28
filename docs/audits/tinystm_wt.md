# Audit: TinySTM WT (Write-Through)

**Score: 4/5** — Close match. Write-through semantics naturally align with TLA+ model's direct-memory-update pattern. Memory ordering sub-score: **2/5** — additionally misses post-CAS version re-read window (C++ re-reads `lock->get()` with acquire after `try_lock()` to get true version). See docs/audits/SUMMARY.md for full MO analysis.

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/tinystm_wt.tla` (PlusCal, 361 lines) |
| C++ header | `backends/tm_impl/tiny_stm/tinystm_wt.hpp` |

## Algorithm Summary

Write-through with encounter-time locking: writes go directly to memory (unlike WBCTL/WBETL which buffer). Undo log records old values for abort. Incarnation bits detect stale reads after abort.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| begin(): clear, start_version=get_clock() | L_idle: state=active, clear readSet/writeSet | ✅ | |
| write_word: first write → lock, write-through, log old | L_active: First write: lock, undoLog, mem update | ✅ | |
| write_word: update → write-through, log old | L_active: Update write: write-through, log old | ✅ | |
| commit: increment_clock → validate RS | L_validateWT: clock+1, validate | ✅ | Split from monolithic commit |
| commit: release locks | L_unlock: release write-set locks with clock | ✅ | Split from monolithic commit |
| abort: restore old values, bump incarnation, unlock | L_abort: restore undo, bump incarnation, unlock | ✅ | Separate label |
| read_word: double-check protocol | L_active: Read: record version+incarnation | ⚠️ Partial | Spin loop + re-read not modeled |
| endVersion / extend check | endVersion[t], L_extend: validate + update endVersion | ✅ | Added per-thread endVersion |
| atomic_signal_fence (seq_cst) | lastFence[t] := "sc" on read/clock-inc/validate | ✅ | FenceFidelity invariant |
| 4-tuple lock encoding (owner, version, incarnation) | lock[a] = <<locked,owner,version,incarnation>> | ✅ |

## Invariants

| Invariant | C++ check | TLC result |
|-----------|-----------|------------|
| MutexLocks | CAS ensures single owner | ✅ PASS |

## Deviations

### 1. Incarnation bit overflow / modulo (Low risk)
**C++** (`tinystm_wt.hpp`): Abort bumps incarnation by 1 with no modulo (the TLA+ model uses `(lock[a][4] + 1) % 8` for bounded model checking).

The modulo is required for TLC's bounded model checking (prevents state-space explosion from unbounded incarnation). The C++ implementation uses a full 8-bit (or larger) field without modulo.

**Risk**: Low — the modulo doesn't affect safety within the bounded model. An incarnation overflow in real C++ would wrap around naturally.

### 2. Read-set stores 3-tuple including incarnation (No risk)
**C++** (`tinystm_wt.hpp`): Read-set records `<<addr, version, incarnation>>`.
**TLA+**: `readSet[self] <<a, v, i>>` — 3-tuple with version and incarnation.

✅ Match.

### 3. Read-set validation checks incarnation (No risk)
**C++**: Validation checks both version and incarnation match.
**TLA+** (L_active commit branch): `lock[a][2] = self \/ (lock[a][3] = v /\ lock[a][4] = i)` — same check.

✅ Match.

### 4. Double-check read protocol (Low risk)
Same as WBCTL/WBETL — spin-loop for locked addresses not modeled.

### 5. Commit split into validate + unlock (Resolved — modeled)
**C++**: Commit has distinct phases: increment_clock, validate read-set, release locks.
**TLA+**: Now split into `L_validateWT` (clock+1, validate) and `L_unlock` (release locks). `L_abort` remains separate.

**Risk**: Resolved — the model now captures interleavings between validation and unlock.

### 6. extend() / version extension (Resolved — modeled)
Same as WBCTL deviation #7 — `endVersion[t]` + `L_extend` now model the extend() semantics.

### 7. Fence annotation precision (Low risk)
Same as WBCTL deviation #8 — `signal_fence` and `thread_fence` both recorded as `"sc"`; `FenceFidelity` checks fence count, not type.

## Summary

| Aspect | Verdict |
|--------|---------|
| Write-through protocol | ✅ Good match (direct mem update matches TLA+ style) |
| Undo log on abort | ✅ Well-modeled with separate L_abort label |
| Incarnation tracking | ✅ 3-tuple read-set matches C++ |
| Commit phases (validate + unlock) | ✅ Split into L_validateWT + L_unlock |
| endVersion + extend() | ✅ Modeled via endVersion[t] + L_extend |
| Fence annotations | ✅ lastFence[t] + FenceFidelity |
| Lock retry / spin | ❌ Not modeled |
| **Overall score** | **4/5** |
