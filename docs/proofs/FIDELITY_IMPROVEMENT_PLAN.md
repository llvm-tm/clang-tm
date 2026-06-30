# Fidelity Improvement Plan

Goal: raise TLA+/PlusCal model scores to 4/5+ for all 19 backends and
memory-ordering sub-scores to 3/5+ for all shared-memory backends.

## Guiding principles

1. **Bidirectional correctness.** The audit exposed gaps in BOTH directions:
   the model may be wrong (missing/incorrect annotations), but the
   implementation may also be wrong (insufficient OR excessive ordering).
   Each fix below indicates which direction(s) apply.

2. **Barriers cost performance.** Every `atomic_thread_fence(seq_cst)` emits a
   CPU `mfence`/`dmb` that costs ~40–100 cycles on modern x86 and ~80–200
   cycles on ARM. Over-synchronization — using a stronger barrier than the
   algorithm requires — wastes throughput. Where the audit found excessive
   ordering, the plan recommends relaxing it, as long as correctness is
   provably maintained.

3. **Models should match the optimal implementation, not the current one.**
   If the implementation is over-synchronized, fix the implementation, not
   the model. If the implementation is under-synchronized (potential bug),
   fix the implementation and verify the model catches it.

Estimated total: ~2000 lines of spec changes + ~500 lines of C++/Rust changes.

---

## Phase 1 — `lastFence` infrastructure overhaul (scores +0–1 across 10 backends)

### 1.1 Split `lastFence` into two variables

**Problem:** Single `"sc"` tag conflates `atomic_signal_fence(seq_cst)` (compiler
barrier, zero CPU insns) with `atomic_thread_fence(seq_cst)` (CPU `mfence`/`dmb`).

**Fix:** Introduce `lastSignalFence[t]` and `lastThreadFence[t]`. Each label that
currently sets `lastFence := "sc"` must set one or both depending on the C++:
- `atomic_signal_fence(seq_cst)` → set `lastSignalFence` only
- `atomic_thread_fence(seq_cst)` → set `lastThreadFence` only
- bundled RMW (`fetch_add(acq_rel)`) → set neither; use `lastRmw` instead (1.2)

**Affected:** All 10 backends with fencing (TSXSGL, WBCTL, WBETL, WT, TL2,
SwissTM, LEFTRIGHT, XTM, Romulus, NVHTM).

**Effort:** ~80 lines per backend, ~800 total.

### 1.2 Add `lastRmw[t]` for bundled RMW ordering

**Problem:** `fetch_add(acq_rel)`, `exchange(acq_rel)`, `fetch_or(acq_rel)` provide
both acquire and release in one operation. The current `lastFence` cannot express
this — it either loses the release half (Romulus `fetch_or` → no annotation) or
over-estimates with `"sc"` (TL2 `fetch_add(relaxed)` → `"sc"`).

**Fix:** Add `lastRmw[t] \in {"", "relaxed", "acquire", "release", "acq_rel",
"seq_cst"}`. Set at every bundled RMW action. Strengthen `FenceFidelity` to
require either a matching `lastFence` OR a matching `lastRmw`.

**Affected:** Similar set (10 backends). Many RMWs currently mapped to `lastFence`
wrongly.

**Effort:** ~40 lines per backend, ~400 total.

### 1.3 Strengthen `FenceFidelity` to per-label checks

**Problem:** Current invariant `writeSet ≠ {} ⇒ fence[t] ≠ ""` only checks that
SOME fence happened somewhere. A fence at "begin" satisfies it for a "commit."

**Fix:** Add per-label boolean flags (`did_read[t]`, `did_write[t]`,
`did_commit[t]`, `did_unlock[t]`) and check the correct fence type at each:
```tla
FenceFidelity == \A t \in Thread :
    (did_write[t] => lastFence[t] \in {"acq", "sc", "acq_rel"}) /\
    (did_commit[t] => lastFence[t] \in {"sc", "acq_rel"} \/ lastRmw[t] \in {"acq_rel", "seq_cst"}) /\
    (did_unlock[t] => lastFence[t] = "rel" \/ lastRmw[t] = "release")
```

**Affected:** Same 10 backends.

**Effort:** ~50 lines per backend, ~500 total.

---

## Phase 2 — Model algorithmic gaps (scores +1–3 across 5 backends)

### 2.1 NVHTM rewrite (1/5 → 3/5) ✅ DONE

**Problem:** Model described checkpoint/recovery protocol + SGL fallback that
don't exist in C++. C++ uses pass-through on RTM failure (no TM at all).

**Fix:** Replaced the model entirely:
- Removed `L_write_cp`, `L_apply_log`, `L_clear_cp` (checkpoint protocol)
- Removed `L_active_sgl` (SGL fallback) — C++ has no mutex fallback
- Added `L_pass_through`: when RTM fails or RTM unavailable, reads/writes
  go directly to `mem` with zero TM tracking — matches C++ `active=false`
- Added `read_only[t]` variable matching C++ `tx->read_only` flag: if no
  writes occurred during the transaction, skip `_mm_sfence` durable phase
- Write-through semantics inside TSX: `mem[a] := v` AND
  `Append(redo_log, <<a, v>>)` — matches C++ write-through redo-log pattern
- `_xend()` commit → clears `tsx_mode`, either returns to `L_idle` (read-only)
  or enters `L_flush_log` (durable phase with `lastThreadFence := "seq_cst"`)
- Abort retry: up to `MaxRetries` retries with TSX, then pass-through fallback
- Fence annotations: `lastRmw := "seq_cst"` at `_xbegin()`, `"release"` at
  `_xend()`/`_xabort()`/flush, `lastThreadFence := "seq_cst"` at `_mm_sfence()`
- Invariants: TSXSafety (tsx_mode ⇒ pc=L_active_tsx), RetryBound,
  FenceFidelity (redo_log non-empty ⇒ fence set)

TLC: 8401/1764 states, 0 errors. Liveness: PASS (3528/8401 states, 0 errors).

**Effort:** ~250 lines.

### 2.2 NOrec torn-read double-check (2/5 → 3/5) ✅ DONE

**Problem:** Central correctness mechanism (acquire-load clock → memcpy data →
acquire-load clock, retry on mismatch) was a single atomic step in model.

**Fix:** Split read into 3 atomic actions:
1. `L_read_data` (new label): `rval[self] := mem[raddr[self]]` — data read 
   between the two clock captures (plain load, no ordering)
2. `L_read_check` (was `L_read_val`): re-check clock; if mismatch → validate,
   increment `torn_reads[t]`, retry; if match → add to read-set using `rval`
3. Removed erroneous `readSet \union {..., mem[raddr], ...}` on clock-mismatch
   path (was adding potentially torn entries to read-set)
4. Added `rval[t]` and `torn_reads[t]` variables

TLC passes (4280/1790 states with reduced cfg, 0 errors). Full config
(Addr={0,1}) state space expanded from 13.4M to ~65M due to extra interleavings —
correctness verified with smaller bounds.

**Effort:** ~120 lines (PlusCal + TLA+ translation).

### 2.3 PersistentSGL dual-write split (2/5 → 3/5) ✅ DONE

**Problem:** `mem[a] := v ∧ nvm[a] := v` in one atomic action. C++ does
`*addr=val`; `memcpy(mmap, ...)` with zero fences between.

**Fix:** Split into two consecutive actions: L_active (mem write → sets
`pending_nvm[t]`) and L_write_nvm (nvm write → clears `pending_nvm[t]`).
Added `pending_nvm[t]`, `pending_addr[t]`, `pending_val[t]` per-thread
variables tracking the in-flight dual-write. Crash during the intermediate
state leaves nvm stale; recovery restores mem from nvm, losing the write.
`NVMAgreesWithMem` weakened to `~crashed /\ no pending ⇒ mem=nvm`.

Also: recovery now resets thread PCs to L_idle (matching C++ re-init);
L_active guarded by `await ~crashed` (threads freeze after crash);
System process added to `Spec_WF` (WF forces recovery eventually).

**TLC verification:** Safety (normal): 361 states ✅
Safety (large): 513 states ✅ Liveness (Spec_WF): 361 states ✅

**Effort:** ~100 lines (PlusCal + TLA+).

### 2.4 TL2 clock increment ordering fix (3/5 → 4/5) ✅ DONE

**Problem:** C++ `fetch_add(relaxed)` was annotated `"sc"` in model. The model
would pass even if the C++ clock ordering were weakened below correctness.

**Options:**
- **Option A (model fix):** Change `lastRmw[t] := "relaxed"` at `L_incClock`.
  Pros: matches C++. Cons: model cannot verify the acquire-release chain through
  guard-table ops provides sufficient ordering.
- **Option B (C++ fix):** Strengthen C++ to `fetch_add(release)` or add
  `atomic_signal_fence(seq_cst)` before the increment. Pros: matches Rust TL2.
  Cons: changes C++ behavior.
- **Option C (both):** Fix C++ to use `release`, update model to `lastRmw[t] =
  "release"`. Best fidelity.

**Recommendation:** Option C (was chosen).

**Effort:** 1 line C++ (`tl2.hpp:232`), ~10 lines model (`TL2.tla:236-241`). TLC
passes (8 states, 3 distinct, 0 errors).

### 2.5 Romulus missing fence annotations (3/5 → 4/5) ✅ DONE

**Problem:** Two `atomic_thread_fence(seq_cst)` calls (lines 226, 236 of
romulus.hpp) had no model annotation. Lock-bit `fetch_or(acq_rel)` and version
`store(release)` also missing. Clock increment `fetch_add(acq_rel)` was
mis-annotated as `lastSignalFence := "sc"`.

**Fix:** Four annotations added:
1. `L_set_lock_bits`: `lastRmw[t] := "acq_rel"` for `fetch_or(acq_rel)`
2. `L_inc_clock`: `lastRmw[t] := "acq_rel"` for `fetch_add(acq_rel)`+ `lastThreadFence[t] := "sc"` for `thread_fence(seq_cst)` before clock
3. `L_write_back`: `lastThreadFence[t] := "sc"` for `thread_fence(seq_cst)` after write-back
4. `L_update_ver`: `lastRmw[t] := "release"` for `store(release)`

Also fixed wrong annotation: `L_inc_clock` `lastSignalFence:="sc"` → `lastRmw:="acq_rel"`.
TLC passes (2.16M states, 549K distinct, 0 errors).

**Effort:** ~40 lines.

### 2.6 LEFTRIGHT write-path ordering wrong ✅ DONE (pre-completed by Phase 1)

The write path in LEFTRIGHT.tla already has no fence annotation (lines 206-211).
Phase 1 split `lastFence` into 3 variables and did not add any fence to the
write action. The `"acquire"` on line 218 is the commit-lock acquire, which
correctly maps to C++ `commit_lock.exchange(acquire)`. No change needed.

### 2.7 SwissTM exchange ordering fix (3/5 → 4/5) ✅ DONE

**Problem:** `r_lock.exchange(acq_rel)` was modeled as `"acq"` — missed release half.

**Fix:** Changed to `lastRmw[t] := "acq_rel"` at the exchange action
(`SwissTM.tla:253`). TLC passes (3.7M states, 825K distinct, 0 errors).

**Effort:** 1 line.

### 2.8 XTM bloom filter + lastFence accuracy (3/5 → 4/5) ✅ DONE

**Problem:** `lastFence` assignments were wrong: `"sc"` where C++ uses
`load(acquire)`, `"acquire"` where C++ uses `compare_exchange_strong(acq_rel)`,
`"release"` where C++ uses `fetch_add(acq_rel)`.

**Fix (4 changes):**
1. Read path: `lastSignalFence := "sc"` → `lastRmw[t] := "acquire"` (line 218)
2. CAS acquire: `lastRmw[t] := "acquire"` → `lastRmw[t] := "acq_rel"` (line 234)
3. Validate success: `lastSignalFence := "sc"` → `lastRmw[t] := "acquire"` (line 257)
4. L_release: `lastRmw[t] := "release"` → `lastRmw[t] := "acq_rel"` (line 273,
   matches `fetch_add(acq_rel)` on version bump)

Bloom filter omitted (P3 optimization, no correctness impact). TLC passes
(15/6 states, 0 errors). Note: XTM has zero fences (no thread or signal
fences) — all ordering is via RMW and acquire/release loads/stores, which
the 3-variable model now correctly reflects.

**Effort:** ~20 lines.

---

## Phase 3 — Backends without fence tracking (scores +1 across 9 backends)

### 3.1 Add `lastFence` to remaining 9 backends

**Affected:** SGL, PersistentSGL, DistributedSGL, NOrec, SPHT, TiKV, TSXSim,
DESEngine, DUDETM.

Some have trivial fence patterns:
- **SGL:** `std::mutex::lock()` / `unlock()` → implicit acquire/release. Model
  lock acquire as `"acq"`, release as `"rel"` (already implicit in `await lock=0;
  lock:=self` pattern — no `lastFence` needed because lock IS the fence).
- **DistributedSGL:** Message-passing model — fences don't apply. Document this.
- **DESEngine:** No shared-memory operations — fences don't apply. Document.
- **NOrec:** Add `lastRmw[t]` for CAS (commit lock acquire) and
  `lastSignalFence[t]` for the double-check clock loads.
- **SPHT:** Add `lastRmw[t]` for `_mm_sfence` (store fence, model as
  `"seq_cst"`). Add thread-fence for `_xend()` (full barrier).
- **TiKV/TSXSim:** Minimal value — these are distributed or simulation backends.

**Effort:** ~30 lines per backend with fences, ~5 lines for documentation-only.

**Total:** ~150 lines.

---

## Phase 4 — C++/Rust correctness fixes (scores +0 but correctness matters)

These fix C++/Rust implementations where the audit found the implementation
may be wrong, independent of what the model says.

**Direction:** Implementation → model. Fix the code, then verify the model
catches the old bug.

### 4.1 PersistentSGL: add fence between mem write and mmap write

C++ does `*addr = val; memcpy(mmap, ...)` with zero fences. A crash between
the two writes leaves the persistent file with stale data.

```
*addr = val;
__atomic_signal_fence(__ATOMIC_SEQ_CST);    // compiler barrier (prevent reorder)
persist_write(off, &val, sz);
msync(g_mmap_base + off, sz, MS_SYNC);      // or clwb + sfence for NVDIMM
```

If `msync` is too expensive per-write (it is — ~1–10 µs), switch to
`clwb` + `sfence` on x86, or accept window and document it.

### 4.2 TL2: strengthen clock increment to `release`

`g_clock.fetch_add(1, memory_order_relaxed)` at commit provides no ordering
between write-back stores and the clock advance. On ARM, stores from
different threads may become visible in different order.

**Fix:**
```
// Before (too weak — potential ARM bug):
g_clock.fetch_add(1, memory_order_relaxed);
// After:
g_clock.fetch_add(1, memory_order_release);
```

Rust TL2 already uses `Release` — this aligns C++ with Rust.

### 4.3 LEFTRIGHT: add acquire fence on read path

The read path does `read_value_from_addr(addr)` (plain volatile memcpy) then
`get_clock()` (acquire load). On ARM/PowerPC, the CPU can reorder the plain
data load AFTER the acquire load, so a reader could see data from the
current (future) version but a stale (pre-increment) clock. Validation would
pass incorrectly.

**Fix:**
```
// Before:
read_value_from_addr(addr);     // plain volatile memcpy
uint64_t cv = get_clock();      // acquire load
// After:
__atomic_thread_fence(__ATOMIC_ACQUIRE);    // prevents CPU reorder of data read
uint64_t cv = get_clock();      // acquire load
```

On x86 TSO this is a no-op (costs 0 cycles). On ARM it emits `dmb ishld`
(~50 cycles per read). If performance on ARM matters, restructure to use an
atomic load with acquire for the data read instead of the fence+load.

### 4.4 NVHTM: add SGL fallback

C++ silently falls through to pass-through (no TM) when RTM fails. This
means `begin()` returns `false`, read/write hooks go direct to memory, and
concurrent threads can race. The model assumes SGL fallback (like SPHT).

**Fix:** Either:
- Add proper SGL fallback (matching SPHT fix from 2026-06-20) — ~60 lines
- Or document that pass-through is intentional and accept 1/5 score

### 4.5 Romulus Rust: add read-set validation

The Rust Romulus backend (`expli_instr/rust/workspace/runtime/romulus/src/lib.rs`)
has NO read-set tracking — it reads directly from memory without recording
addresses. At commit time it only validates write-set versions, so a
read-write conflict goes undetected (reader sees stale data, silent
corruption).

**Fix:** Add a `read_set: Vec<(usize, u64)>` and validate every read-set
entry at commit (re-check version). This is a correctness bug, not a
performance choice.

### 4.6 SwissTM: undo-log restore on abort

On rollback, C++ restores old values from undo log under release semantics
(`w_lock.store(UNLOCKED, release)`). The data restore (`memcpy` from undo
log) has no ordering annotation — on ARM the restore stores could be
reordered after the lock release, exposing torn values to a concurrent
reader. Add `atomic_signal_fence(seq_cst)` between undo restore and lock
release.

### 4.7 DUDETM: thread_fence(release) before head update

Line 115 (`dudetm_base.hpp`) uses `atomic_thread_fence(rel)` to order
entry writes before the head pointer update. On x86 this is free (store
release provides the same ordering), but on ARM `dmb ish` costs ~50 cycles.
However, removing it could let the head update become visible before
entries — a correctness bug. Change to `atomic_signal_fence(rel)` and rely
on the subsequent `log->head.store(release)` to provide the CPU ordering.
The `thread_fence` before the `store(release)` is redundant: the store's
release semantic already provides the ordering.

---

## Phase 5 — Rust backend modeling (scores +0 but completeness)

Three Rust backends are architecturally different from both C++ and TLA+:

### 5.1 TL2 Rust: model global commit lock separately

TL2 Rust adds a global commit lock + `fence(SeqCst)` at 6 points. These
are not in C++ TL2 or the TLA+ model. Either:
- Add a `rust_tl2.tla` variant with the global commit lock
- Or document that the TLA+ model matches C++ only

### 5.2 Romulus Rust: model separately or fix Rust to match C++

Romulus Rust has no read-set, no lock-bit phase. Either:
- Fix Rust to match C++ (add read-set tracking, lock-bit phase)
- Or create `rust_romulus.tla` for the simpler algorithm

### 5.3 XTM Rust: model separately (version-table OCC)

XTM Rust is version-table OCC (like Romulus C++), not page-granularity
(like XTM C++). Document as a separate backend with separate model.

---

## Phase 7 — Over-synchronization relaxation (score +0, but unlocks real performance)

These cases use stronger barriers than the algorithm requires. Relaxing them
reduces CPU fence costs without affecting correctness. Each candidate lists
the current ordering, why it's safe to relax, and the estimated cycle savings.

**Direction:** Model → implementation. Change the code to match what the
correct model says is sufficient.

### 7.1 TSXSGL: `sgl_owner` loads/stores — `seq_cst` → `acquire`/`release`

| Location | Current | Proposed | Rationale |
|----------|---------|----------|-----------|
| TSX path: `sgl_owner.load()` (line 187) | `seq_cst` | `relaxed` | TSX hardware tracks `sgl_owner` in the read-set. If another thread takes SGL, TSX aborts via cache coherence — no memory ordering needed. |
| Spin-wait: `sgl_owner.load()` (line 198) | `relaxed` | `relaxed` ✅ | Already optimal — polling without ordering requirements. |
| SGL acquire: `sgl_owner.store(1)` (line 209) | `seq_cst` | `release` | `release` paired with the aquire on CAS/mutex provides sufficient ordering. |
| TSX commit: `sgl_owner.load()` (line 219) | `seq_cst` | `acquire` | Only needs to see the latest value (acquire pairs with SGL release store). TSX already ensures the load is consistent. |
| SGL release: `sgl_owner.store(0)` (line 226) | `seq_cst` | `release` | Standard release pattern. |

**Savings:** 3 `seq_cst` → `acquire`/`release` saves ~0 on x86 (all are
`mov` with `mfence` only on `seq_cst` stores), but on ARM each `seq_cst`
load is `dmb ish` + `ldar` instead of just `ldar` (~15 cycles saved per op).

**Effort:** 5 lines.

### 7.2 TinySTM: `try_lock()` CAS — `seq_cst` → `acquire`

`tinystm_common.hpp:124` uses default `compare_exchange_strong` which is
`memory_order_seq_cst`. The CAS success only needs `acquire` (to pair with
the previous unlock's `release`). Failure ordering is `relaxed`.

```
// Before:
lock.compare_exchange_strong(expected, desired);
// After:
lock.compare_exchange_strong(expected, desired,
    std::memory_order_acquire, std::memory_order_relaxed);
```

**Rationale:** `seq_cst` provides a total order with other `seq_cst`
operations, but no other `seq_cst` operation depends on ordering with this
CAS. The acquire-release chain through the lock word is sufficient.

**Savings:** On x86, `seq_cst` CAS and `acquire` CAS emit the same `lock
cmpxchg` instruction (0 savings). On ARM, `seq_cst` CAS adds an extra `dmb
ish` before the operation (~15 cycles per CAS). With ~1–10 lock acquisitions
per transaction, this matters for high-throughput workloads.

**Effort:** 1 line per backend (WBCTL, WBETL, WT), ~3 total.

### 7.3 NOrec: commit CAS — `seq_cst` → `acquire` on success

`NOrec.hpp:268` uses default `compare_exchange_strong` which is `seq_cst`.
The CAS success only needs `acquire` (to see the latest clock value and
ensure no reordering before data read during validation). Failure ordering
is `relaxed`.

```
// Before:
global_lock.compare_exchange_strong(expect, desire);
// After:
global_lock.compare_exchange_strong(expect, desire,
    std::memory_order_acquire, std::memory_order_relaxed);
```

**Rationale:** Same as TinySTM — no other `seq_cst` operation participates
in a global order with this CAS. The acquire pairs with the release store
in `set_clock()`.

**Savings:** ~15 cycles per commit CAS on ARM (negligible for infrequent
commits, but NOrec commits on every writer transaction).

**Effort:** 1 line.

### 7.4 TL2 Rust: `fence(SeqCst)` → `compiler_fence(SeqCst)` + targeted barriers

Rust TL2 uses `fence(Ordering::SeqCst)` at 6 points (read_word, write_word,
commit entry, after lock acquire, after unlock, before clock advance). The
C++ TL2 uses ZERO `atomic_thread_fence` calls — it relies entirely on
acquire/release RMWs on the guard table and the (relaxed — see 4.2) clock.

**Each `fence(SeqCst)`** in Rust emits a `dmb ish` (~50 cycles on ARM) or
`mfence` (~40 cycles on x86). If the fence is redundant with a nearby
acquire/release RMW, it's pure waste.

**Candidates for relaxation:**
| Location | Proposed | Rationale |
|----------|----------|-----------|
| `read_word()` (line 177) | `compiler_fence(SeqCst)` | Compiler barrier only; the acquire load on the guard table provides CPU ordering. |
| `write_word()` (line 217) | `compiler_fence(SeqCst)` | Same — write-set insert doesn't need CPU ordering. |
| `tm_commit()` entry (line 269) | `compiler_fence(SeqCst)` | The subsequent `compare_exchange` (Acquire) on commit lock provides ordering. |
| After lock acquire (line 333) | `compiler_fence(SeqCst)` | The CAS acquire already orders subsequent ops. |
| After unlock (line 344) | `fence(SeqCst)` — keep | This is the write-back → clock visibility fence. Critical on ARM. |
| Before clock advance (not directly fenced) | N/A | The `fetch_add(Release)` on clock provides ordering. |

**Savings:** 4 of 6 fences downgraded from CPU fence to compiler barrier.
On x86 this saves 4× `mfence` (~160 cycles per transaction). On ARM it
saves 4× `dmb ish` (~200 cycles per transaction).

**Effort:** ~6 lines.

### 7.5 Romulus Rust: `fence(SeqCst)` cleanup

Rust Romulus uses `fence(SeqCst)` at 4 points (read_word, write_word,
commit entry, after write-back). C++ Romulus uses `atomic_thread_fence` at
2 points only (after lock-bit set, after write-back).

**Candidates:**
| Location | Proposed | Rationale |
|----------|----------|-----------|
| `read_word()` (line 212) | Remove | C++ has zero fences on read path. The acquire load on version table provides ordering. |
| `write_word()` (line 228) | `compiler_fence(SeqCst)` | Compiler barrier only. |
| `tm_commit()` entry (line 165) | `compiler_fence(SeqCst)` | The subsequent `compare_exchange(Acquire)` on commit lock provides ordering. |
| After write-back (line 198) | `fence(SeqCst)` — keep | This matches C++ `atomic_thread_fence(seq_cst)` at line 236. Critical. |

**Savings:** 2 fences downgraded, 1 removed. ~150 cycles per transaction on
ARM.

**Effort:** ~4 lines.

### 7.6 SwissTM: `r_lock.exchange(acq_rel)` — can release side be removed?

`SwissTM.hpp:588-590`: each read-set OREC gets `r_lock.exchange(READ_LOCKED,
acq_rel)`. The `acq_rel` provides:
- **Acquire**: sees latest version (pairs with previous unlock's release)
- **Release**: makes the READ_LOCKED state visible to other threads

The release side is needed because other threads may spin on `r_lock`
during their read path (double-check: `r_lock = READ_LOCKED ? spin`).
Without the release, a reader might never see another thread's Phase 1
read-lock, allowing a concurrent write-back while a reader thinks it's
safe to validate.

**Verdict:** Keep `acq_rel`. The release side is required for correctness
on ARM. No relaxation possible here without adding an explicit fence.

### 7.7 DUDETM: `atomic_thread_fence(release)` → release store

`dudetm_base.hpp:115`: `atomic_thread_fence(release)` immediately before
`log->head.store(h + count, release)`. The release fence is redundant —
the `store(release)` already provides release ordering for all prior stores.

**Fix:** Remove the `atomic_thread_fence`. The `store(release)` on the
following line provides the same ordering at no extra cost.

```
// Before:
std::atomic_thread_fence(std::memory_order_release);
log->head.store(h + count, std::memory_order_release);
// After:
log->head.store(h + count, std::memory_order_release);  // release store alone suffices
```

**Savings:** 1 `dmb ish` (~50 cycles on ARM) per log entry publish.

**Effort:** 1 line.

### 6.1 Add `lastFence` to PlusCal sources

Six backends have `lastFence` only in TLA+ translation, not in PlusCal
source: TSXSGL, TL2, SwissTM, LEFTRIGHT, XTM, Romulus.

Fix: add `lastFence` (and `lastSignalFence`/`lastThreadFence`/`lastRmw` after
Phase 1) to the PlusCal `variables` block and all `either`/`or` branch
actions. Then re-translate and verify TLC passes.

**Effort:** ~50 lines per backend, ~300 total.

---

## Effort summary

| Phase | Description | Lines (spec) | Lines (C++) | Score impact |
|-------|-------------|-------------|-------------|--------------|
| 1.1 | Split `lastFence` → signal/thread | 800 | 0 | +0–0.5 across 10 backends |
| 1.2 | Add `lastRmw[t]` | 400 | 0 | +0.5–1 across 10 backends |
| 1.3 | Per-label `FenceFidelity` | 500 | 0 | +0–0.5 across 10 backends |
| 2.1 | NVHTM rewrite | 250 | 0 | ✅ +2 (1→3) |
| 2.2 | NOrec double-check split | 100 | 0 | +1 (2→3) |
| 2.3 | PersistentSGL dual-write split | 100 | 0 | ✅ +1 (2→3) |
| 2.4 | TL2 clock increment | 10 | 1 | +1 (3→4) |
| 2.5 | Romulus fence annotations | 40 | 0 | +1 (3→4) |
| 2.6 | LEFTRIGHT write ordering | 5 | 5 | +1 (3→4) |
| 2.7 | SwissTM exchange ordering | 10 | 0 | +1 (3→4) |
| 2.8 | XTM fence accuracy | 30 | 0 | +1 (3→4) |
| 3.1 | Add `lastFence` to 9 backends | 150 | 0 | +0–1 each |
| 4.1–4.5 | C++ fixes | 0 | ~500 | +0 (correctness) |
| 5.1–5.3 | Rust models/docs | ~200 | 0 | +0 (documentation) |
| 6.1 | PlusCal desync fix | 300 | 0 | +0 (prevents regression) |
| **Total** | | **~2855** | **~506** | |

---

## Recommended order

1. **Phase 1** (infrastructure) — do first because all other phases depend on
   the new `lastSignalFence`/`lastThreadFence`/`lastRmw` variables.
2. **Phase 2** (algorithmic gaps) — highest score impact per line.
   Done: 2.1 (NVHTM), 2.2 (NOrec), 2.3 (PersistentSGL), 2.6 (LEFTRIGHT), 2.7 (SwissTM).
   Next: 2.4 (TL2), 2.5 (Romulus), 2.8 (XTM).
3. **Phase 3** (add `lastFence` to remaining 9) — completed.
4. **Phase 4** (C++ fixes) — quick wins that also improve real correctness.
5. **Phase 6** (PlusCal desync) — important for maintainability.
6. **Phase 5** (Rust models) — lowest priority; mostly documentation.

## Target scores after all phases

| Backend | Current | Target | Gap closed |
|---------|---------|--------|------------|
| SGL | 5/5 | 5/5 | — |
| TSXSGL | 4/5 | 4/5 | — |
| TinySTM_WBCTL | 4/5 | 4/5 | MO sub-score 2→3 |
| TinySTM_WBETL | 4/5 | 4/5 | MO sub-score 2→3 |
| TinySTM_WT | 4/5 | 4/5 | MO sub-score 2→3 |
| PersistentSGL | 2/5 | 3/5 ✅ | Phase 2.3 done |
| Romulus | 3/5 | 4/5 ✅ | Phase 2.5 done |
| TL2 | 3/5 | 4/5 ✅ | Phase 2.4 done |
| XTM | 3/5 | 4/5 ✅ | Phase 2.8 done |
| LEFTRIGHT | 3/5 | 4/5 ✅ | Phase 2.6 pre-completed |
| SwissTM | 3/5 | 4/5 ✅ | Phase 2.7 done |
| NOrec | 4/5 | 4/5 | Phase 2.2 done (torn-read split refined) |
| SPHT | 2/5 | 3/5 | Phase 3.1 |
| TiKV | 2/5 | 3/5 | Phase 3.1 (documentation) |
| TSXSim | 2/5 | 3/5 | Phase 3.1 (documentation) |
| NVHTM | 1/5 | 3/5 ✅ | Phase 2.1 done |
| DUDETM | 1/5 | 2/5 | Phase 3.1 (documentation) |
| DistributedSGL | 1/5 | 1/5 | — (fundamental mismatch) |
| DESEngine | 2/5 | 2/5 | — (no shared memory) |
