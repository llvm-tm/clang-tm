# TLA+ Spec Soundness Review — Recommendations

## Priority 0: Memory ordering semantics (affects all 19 backends)

### 0.1 `lastFence[t]` cannot distinguish signal_fence from thread_fence (all 10 backends with fence tracking)

All 10 backends with `lastFence` use a single tag (`"sc"`) for both `atomic_signal_fence(seq_cst)` (compiler barrier, emits zero CPU instructions) and `atomic_thread_fence(seq_cst)` (CPU `mfence`/`dmb`). On x86 this doesn't matter (`signal_fence` and `thread_fence` are both compiler barriers because x86 TSO provides strong ordering), but on ARM/PowerPC the difference is critical — `signal_fence` provides no hardware ordering. The model cannot catch bugs where a `signal_fence` is insufficient.

**Proposed fix:** Split `lastFence` into `lastSignalFence[t]` and `lastThreadFence[t]`. `FenceFidelity` should check BOTH where appropriate. Alternatively, add a new `lastFence` value `"thread_sc"` distinct from `"sc"`.

### 0.2 `lastFence[t]` cannot distinguish load-acquire from store-release from bundled acq_rel RMW (all 10 with fence tracking)

All three ordering patterns are collapsed:
- `guard.load(acquire)` → `lastFence := "sc"` (wrong — acquire is weaker than seq_cst)
- `lock.store(release)` → `lastFence := "rel"` (correct classification)
- `fetch_add(acq_rel)` → `lastFence := "sc"` (wrong — bundled ordering is neither pure acquire nor pure release)
- `r_lock.exchange(acq_rel)` → `lastFence := "acq"` (wrong — misses the release half)

**Proposed fix:** Add a separate `lastRmw[t]` variable with values `"acq"`, `"rel"`, `"acq_rel"`, `"relaxed"` to capture bundled RMW ordering independently from `lastFence`.

### 0.3 `FenceFidelity` checks presence, not correctness (all 10 with fence tracking)

The current invariant `\A t \in Thread : writeSet[t] # {} => lastFence[t] # ""` only checks that SOME fence happened somewhere. It does NOT check:
- The fence is at the correct label (a fence at begin is counted for a commit)
- The fence has the correct strength (acq vs rel vs seq_cst)
- The fence is correctly placed relative to the operations it must order
- The fence pairs correctly with another thread's fence (acquire-release pairing)

**Proposed fix:** Strengthen `FenceFidelity` to check per-label requirements, e.g.:
```tla
FenceFidelity == \A t \in Thread :
    (write_action[t] => lastFence[t] = "acq") /\
    (commit_action[t] => lastFence[t] \in {"sc", "acq_rel"}) /\
    (unlock_action[t] => lastFence[t] = "rel")
```
This requires adding boolean flags per label recording which action was taken.

### 0.4 NVHTM model describes algorithm that does not exist in C++

**Severity: Critical.** The model has checkpoint/recovery protocol (`L_write_cp`, `L_apply_log`, `L_clear_cp`) and SGL fallback (`L_active_sgl`) — neither exists in C++. The C++ uses pass-through mode on RTM failure (no TM at all). 12 `lastFence` annotations mostly correspond to nothing in the implementation.

**Fix:** Rewrite the model to match the actual C++ (no checkpoint protocol, pass-through on RTM failure, no SGL fallback), or rename the file and mark it as "aspirational."

### 0.5 NOrec torn-read double-check not modeled

The C++ NOrec central correctness mechanism is the double-check loop:
```
clock1 = acquire_load(global_lock);  // version capture
read_data(addr);                      // plain memcpy
clock2 = acquire_load(global_lock);  // re-check
if (clock1 != clock2) retry;         // torn read detected
```
The model does this as a single atomic action: `readSet := readSet \union {<<a, mem[a], clk>>}`. The model cannot detect torn reads.

**Fix:** Split the read into three atomic steps (clock capture, data read, re-check) with an abort/retry on mismatch. This would roughly double the state space but is necessary to verify NOrec's central correctness property.

### 0.6 PersistentSGL dual-write is atomic in model, non-atomic in C++

The model writes `mem[a] := v ∧ nvm[a] := v` in a single action. The C++ does `*addr = val` then `memcpy` to mmap with NO fence between. `NVMAgreesWithMem` holds in the model but NO crash-consistency guarantee exists in C++.

**Fix:** Remove simultaneous dual-write. Add a crash-possible intermediate state where `mem[a] ≠ nvm[a]`. Then `NVMAgreesWithMem` becomes non-trivial and catches the C++ durability gap.

### 0.7 LEFTRIGHT write path has zero ordering in C++, but model annotates "acq"

The C++ LEFTRIGHT `write_word()` (leftright.hpp:264-275) has zero atomic operations, zero fences. The model sets `lastFence := "acq"` on write, creating a false sense of ordering. This is the only backend where the model OVER-estimates fence coverage on the write path.

**Fix:** Change `lastFence` on write to `""` (no fence), and document that LEFTRIGHT's write path relies on the commit lock and clock increment for ordering.

### 0.8 TL2 C++ clock increment is `relaxed`, model annotates `"sc"`

`tl2.hpp:232`: `g_clock.fetch_add(1, memory_order_relaxed)`. The model tags `L_incClock` with `lastFence := "sc"`. The relaxed clock increment provides zero memory ordering — the model would pass FenceFidelity even if the C++ were weakened below correctness.

**Fix:** Change `lastFence` on clock increment to `""` (matching C++ relaxed), or strengthen the C++ to use `acq_rel` (as per Rust TL2). Either way, model and implementation must agree.

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
| P0 (memory ordering) | 8 | 0 | 8 (all — see individual backends in SUMMARY.md) |
| P1 (blocks TLC) | 3 | 3 ✅ | 0 |
| P2 (wrong inv) | 6 | 5 | 1 (DUDETM `LogWriteMatch`) |
| P3 (suspicious) | 5 | 2 | 3 (lastFence desync, NVHTM recovery, SPHT recovery) |
| P4 (docs) | 4 | 0 | 4 (dead vars, XTM rename, ProgressProperty, NVMAgrees) |

Fixes applied in commit `71b98ae` (recommendations document) and `<PENDING>` (this session's memory ordering audit).
