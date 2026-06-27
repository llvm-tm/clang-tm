# TLA+ Spec Soundness Review — Recommendations

## Priority 1: Critical (blocks TLC execution or allows incorrect behavior)

### 1.1 TSXSim.cfg — Undefined invariant `WriteSetConsistent`

`.cfg` files (both `.cfg` and `-liveness.cfg`) reference `WriteSetConsistent` in the
`INVARIANT` block. This invariant is **not defined** anywhere in `TSXSim.tla`.
TLC will fail with `Invariant WriteSetConsistent not defined.`

**Fix:** ✅ Removed `WriteSetConsistent` from both `.cfg` files.

### 1.2 TSXSim — `TransactionProgress` uses primed variables inside `<>`

```
<>(committed[t]' > committed[t] \/ aborted[t]' > aborted[t])
```

Standard TLA+ temporal `<>F` expects a state predicate (no primes). Using
primed variables inside `<>` is non-standard and TLC may reject it or produce
unexpected results.

**Fix:** ✅ Replaced with `<>(pc[t] = "L_idle")` — a plain state predicate.

### 1.3 TL2 — Validation ignores lock bit

`L_validate` (line 108) checks only `GuardVersion(guard[a]) = v`. The real TL2
algorithm checks the full guard word (lock bit + version). An address locked by
a concurrent committer has `guard[a] = MakeGuard(1, v)` — the version check
passes (`v = v`) but the guard has changed. This allows stale commits.

**Fix:** ✅ Changed validation to check `GuardLocked(guard[a]) = 0 /\ GuardVersion(guard[a]) = v`.

**Cascading fix:** The TL2 `FenceFidelity` invariant was violated because the write
action did not set `lastFence`. Added `lastFence[self] := "acq"` to the PlusCal
write action (and corresponding TLA+ translation). Passes TLC (77M+ states, no errors).

## Priority 2: Incorrect or misleading invariants

### 2.1 TL2 — `NoDirtyRead` has wrong quantifier

```
\A t1, t2 \in Thread, a \in Addr : ... => state[t2] = "idle" \/ a \notin writeSet[t2]
```

The universal quantifier `\A t2` makes the invariant fail whenever the lock
holder (for whom both `state="active"` and `a∈writeSet`) is quantified. The
invariant is correctly excluded from `Inv` but had no explanatory comment.

**Fix:** ✅ Added explanatory comment above `NoDirtyRead` definition.

### 2.2 Tautology `LockExclusion` invariants

Four specs define `(x = t1 ∧ x = t2) ⇒ t1 = t2` which is algebraically true
in TLA+ (a single-valued variable cannot equal two different values):

| Spec | Variable | File |
|------|----------|------|
| PersistentSGL | `lock` | line 195-197 |
| TiKV | `kv_locks[k]` | line 364-367 |
| TSXSim | `sgl_lock` | line 506-508 |
| XTM | `xadt_owner[p]` | line 313-316 |

These invariants can **never fail** — they provide zero verification value.

**Fix:** ✅ Added `(* NOTE: Tautology — ... *)` comments above each definition.
No behavioral changes needed (kept for documentation clarity).

### 2.3 PersistentSGL — `NVMAgreesWithMem` is true by construction

```
\A a \in Addr : mem[a] = nvm[a]
```

The dual-write is atomic in the model (`mem` and `nvm` are always updated
together). This is a model artifact, not a verified property. To check
durability semantics, the write would need to be split into non-atomic steps
with crash between them.

**Fix:** Add comment noting this is a model simplification.

### 2.4 TiKV — `CommittedVisible` is vacuous

Antecedent `HasWritten(t, k)` is always false when `pc[t] = "L_idle"` because
write-set is cleared on every path to idle. The invariant is equivalent to
`TRUE`.

**Fix:** Rewrite or remove.

### 2.5 DUDETM — `LogWriteMatch` has universal/existential confusion

```
\A t \in Thread : \A i \in 1..Len(persist_file) :
    persist_file[i][1] = OP_WRITE =>
        \E prev_idx \in 1..i-1 :
            persist_file[prev_idx][1] = OP_COMMIT_BEGIN /\
            persist_file[prev_idx][2] = t
```

The `\A t \in Thread` makes this demand that every OP_WRITE be preceded by an
OP_COMMIT_BEGIN from **every** thread — impossible with >1 thread.
Additionally, OP_WRITE entries are `<<OP_WRITE, a, v>>` (no thread field), so
thread-matching is meaningless.

**Fix:** The existential quantifier should scope over threads, or `t` should
be existentially bound from OP_WRITE (which needs a thread field).

## Priority 3: Suspicious expressions

### 3.1 TSXSGL — `L_idle` Terminate dead-end

When `committed[t] < MaxCommits` at `L_idle`, the else branch sends the thread
to `L_active` with `mode = "idle"`. No transition in `L_active` handles
`mode = "idle"` — the thread is stuck forever, performing reads/writes it can
never commit.

**Fix:** ✅ Changed `else goto L_active` to `else goto L_idle` (both PlusCal
source and TLA+ translation). Thread loops back to try TSX/SGL again.
Passes TLC (467K states, no errors).

### 3.2 PlusCal/TLA+ desync for `lastFence`

The `lastFence` variable and its updates exist only in the TLA+ translation
output, not in the PlusCal source. Regenerating the translation would silently
lose all fence tracking. Affected: LEFTRIGHT, all TinySTMs, TL2, Romulus,
SwissTM, XTM, TSXSGL.

**Fix:** Add `lastFence` to each PlusCal source's variable declaration and
`either` branch actions, or document that fence tracking must be re-added
manually after any `pcal.trans` run.

### 3.3 Romulus — `VersionEntryValid` excluded from `Inv` without comment

Defined at line 420-426 but not included in `Inv` (line 442). No comment
explains why.

**Fix:** ✅ Added `(* NOTE: Excluded from Inv below ... *)` comment above
definition.

### 3.4 NVHTM — Recovery uses `CHOOSE` instead of `LastIdx`

Recovery (`L_recover` action) uses `CHOOSE i \in 1..Len(redo_log[self]) : ...`
which picks an **arbitrary** matching index. If the redo log has multiple
entries for the same address (possible since append doesn't dedup), recovery
applies the wrong value. The commit-time apply path (`L_apply_log`) correctly
uses `LastIdx`.

**Fix:** Change recovery to use `LastIdx` (same pattern as commit-time apply).

### 3.5 SPHT — `tsx_buffer` not reset on recovery

Recovery path omits clearing `tsx_buffer`. Stale speculative writes from before
the crash could be committed after recovery if the thread enters TSX mode and
commits without writing.

**Fix:** Add `tsx_buffer[self] := [a \in Addr |-> NoWrite]` to the recovery
action.

## Priority 4: Documentation

### 4.1 Comment for `NoDirtyRead` exclusion (TL2)

Add `(* This invariant is excluded from Inv because ... *)` above
the `NoDirtyRead` definition.

### 4.2 Dead state variables

Several variables are written but never read:

| Spec | Variable | Purpose |
|------|----------|---------|
| TiKV | `snapshot`, `commit_ts`, `prewrite_ok` | Set in transitions, never referenced |
| PersistentSGL | `version` | Incremented, never read by any guard or invariant |
| NOrec | `rsSnapshot[t]` | Set on begin, never read |
| DUDETM | `batch_marker` | Declared, never written or read |

**Fix:** Either use them in invariants or remove them.

### 4.3 `VersionMonotonic` misnamed (XTM)

Checks `xadt_version[p] >= 0` (non-negativity). The name implies it checks
monotonicity (versions never decrease).

**Fix:** Rename to `VersionNonNegative` or strengthen to check monotonicity.

### 4.4 `ProgressProperty` references nonexistent label `"L_begin"`

Affects SGL, Romulus, TL2, LEFTRIGHT `ProgressProperty` — references
`"L_begin"` which does not exist in those models.

**Fix:** Remove `"L_begin"` from the set.

## Status

| Priority | Total | Fixed | Remaining |
|----------|-------|-------|-----------|
| P1 (blocks TLC) | 3 | 3 ✅ | 0 |
| P2 (wrong inv) | 6 | 5 | 1 (DUDETM `LogWriteMatch`) |
| P3 (suspicious) | 5 | 2 | 3 (lastFence desync, NVHTM recovery, SPHT recovery) |
| P4 (docs) | 4 | 0 | 4 (dead vars, XTM rename, ProgressProperty, NVMAgrees) |

Fixes applied in commit `71b98ae` (recommendations document) and `<PENDING>` (this session's implementation).
