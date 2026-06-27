# NOrec (NO-Read-Check) STM — Implementation vs. TLA+ Model Audit

**Score: 3/5** — Core algorithm is correctly captured and model-checked, but several meaningful abstraction gaps exist (no torn-read model, no fence tracking, no plugin-bypass modeling, `Serializable` property declared but not in default config). Model fidelity is moderate.

---

## Files

| Artifact | Path | Lines | Role |
|----------|------|-------|------|
| TLA+ Spec | `docs/proofs/NOrec.tla` | 330 | Formal model with TLC config & TLAPS proof sketch |
| TLC Config | `docs/proofs/NOrec.cfg` | 17 | 2-thread, 2-address, 4 invariants |
| C++ Header | `backends/tm_impl/norec/NOrec.hpp` | 675 | Core algorithm: begin/read/write/commit/validate |
| C++ Runtime | `backends/tm_impl/norec/NOrec_runtime.cpp` | 409 | Hook registration, LLVM_TM_PLUGIN lifecycle, allocators |
| C++ Globals | `backends/tm_impl/norec/NOrec_globals.hpp` | 16 | Shared `global_lock`, `thr_counter`, TLS state |

---

## Algorithm Summary

NOrec (Dice, Shavit, EuroSys 2006) is an optimistic concurrency control STM that uses a single global version clock (even = unlocked, odd = locked). Reads are logged with version snapshots and validated on clock changes; writes are buffered in a write-set. On commit, the transaction acquires the global clock via CAS, writes buffered values to memory, then releases the clock (advancing the global version by 2). A clock double-check pattern around each read prevents torn values from concurrent write-back.

---

## Cross-Reference Checklist

Mapping C++ functions/patterns to TLA+ actions/labels.

| # | C++ Function / Pattern | TLA+ Label / Action | TLA+ Lines | Match | Notes |
|---|------------------------|---------------------|------------|-------|-------|
| 1 | `norec::init()` / `tm_init()` | `Init` | 60–73 | ✅ | C++ `global_lock.store(0)` matches `clk = 0`; TLA+ initialises `mem` to zero values |
| 2 | `norec::begin()` — spin until `get_clock() & 1 == 0`, capture snapshot | `Begin(t)` | 78–87 | ✅ | Spin (`clk % 2 = 0`), snapshot = clk, clear read/write sets |
| 3 | `read_word_norec()` — write-set lookup (most-recent-first scan) | `ReadOwnWrite(t, a)` | 92–96 | ✅ | Both check `a \in writeSet[t]`; C++ iterates from end for latest value |
| 4 | `read_word_norec()` — type-interchange fallback (POINTER↔UINT64, wider→narrower, byte-merge) | — | — | ❌ | **Not modeled.** TLA+ has no type system; every address holds a single `Data` value |
| 5 | `read_word_norec()` — clock double-check (`clock_before` → read → `clock_after`) | — | — | ⚠️ | **Not modeled.** TLA+ assumes atomic memory reads; C++ must protect against torn 8-byte reads across concurrent write-back |
| 6 | `read_word_norec()` — validate path (snapshot changed → `validate()` → re-read) | `ReadWithValidation(t, a)` | 106–116 | ✅ | Both validate all read-set entries (`mem[RS_ADDR(e)] = RS_VAL(e)`) then advance snapshot |
| 7 | `read_word_norec()` — fast path (clock unchanged after double-check) | `ReadFromMemory(t, a)` | 98–104 | ⚠️ | TLA+ guard `clk = snapshot[t]`; C++ uses `tx->snapshot != get_clock()` **negated** (proceeds directly on match after double-check). Order differs: TLA+ checks clock FIRST, C++ double-check FIRST then snapshot check |
| 8 | `abort_tx()` — `siglongjmp(*jmpbuf, 1)` | `ReadAbort(t, a)` | 118–127 | ✅ | Both clear read/write sets, increment abort counter, return to idle |
| 9 | `write_word_norec()` — buffer in write-set, set `read_only = false` | `Write(t, a, n)` | 135–142 | ✅ | Both add to write-set, record value, clear read-only flag |
| 10 | `write_word_norec()` — same-address update (find existing entry, overwrite) | — | — | ⚠️ | TLA+ uses `writeSet' = writeSet[t] ∪ {a}` (adds unconditionally). C++ updates existing entry or creates new one. Semantically equivalent (write-set tracks which addrs were written) |
| 11 | `commit()` — read-only fast path (`tx->read_only == true`) | `CommitReadOnly(t)` | 147–156 | ✅ | Both skip clock acquire, clear sets, increment commit count |
| 12 | `commit()` — CAS `global_lock.compare_exchange_strong(expect, desire)` | `CommitCAS(t)` | 158–166 | ✅ | Both `clk = snapshot[t]` → `clk' = snapshot + 1`, clear read-set, enter committing state |
| 13 | `commit()` — CAS failure → `validate()` → retry | `CommitCASFail(t)` | 168–177 | ✅ | Both validate, advance snapshot to `clk - (clk % 2)` (nearest even), retry |
| 14 | `commit()` — write-back loop over `tx->write_set` | `CommitWriteBack(t)` | 179–194 | ✅ | Both write `wbBuffer[t][a]` to `mem[a]`, release clock to `snapshot + 2` |
| 15 | `commit()` — `set_clock(expect + 2)` (release) | `CommitWriteBack(t)` step `clk' = clk + 1` | 186 | ⚠️ | C++: `set_clock(snapshot + 2)` → `global_lock` becomes `snapshot+2`. TLA+: `clk' = clk + 1` → since `clk` was `snapshot+1`, the result is `snapshot+2`. **Match in effect, but note C++ uses the pre-CAS snapshot, TLA+ uses post-CAS clk.** |
| 16 | `validate()` — re-read every read-set entry, compare values | Implicit in ReadWithValidation & CommitCASFail | — | ✅ | Both compare `read_value_from_addr(addr, type)` with `observed_val` |
| 17 | `validate()` — spin on odd clock at entry | Implicit in ReadWithValidation & CommitCASFail | — | ✅ | Both wait for even clock (`(time & 1) != 0 → continue`) |
| 18 | `LLVM_TM_PLUGIN` bypass in `read_word_norec()` lines 415–418 | — | — | ❌ | **Not modeled.** C++ bypasses TM tracking entirely for non-TM-region addresses in plugin mode |
| 19 | `LLVM_TM_PLUGIN` bypass in `write_word_norec()` lines 492–497 | — | — | ❌ | **Not modeled.** C++ writes directly to memory without TM tracking in plugin mode |
| 20 | `LLVM_TM_PLUGIN` bypass in `commit()` lines 276–284 | — | — | ❌ | **Not modeled.** C++ skips write-back for non-TM-region addresses |
| 21 | Null/small-address bypass (lines 411–414, 488–491) | — | — | ❌ | **Not modeled.** C++ returns zero / skips writes for `addr < 0x100000` |
| 22 | Memory ordering (`memory_order_acquire`/`release` on `global_lock`) | — | — | ⚠️ | **Not modeled.** TLA+ has no `lastFence[t]` tracking (unlike TinySTM models which were updated in 2026-06-23) |
| 23 | `real_tm_begin()` / `real_tm_end()` hook wrappers | — | — | ✅ | Runtime wrappers do spec alloc / deferred free management orthogonal to TM protocol |
| 24 | `real_tm_free()` deferred free path (lines 292–295) | — | — | ✅ | Free within transaction appends to deferred list — not modeled but orthogonal |

---

## Invariants

TLC results (`.cfg` includes `ClockParityInv`, `CommitInvariant`, `WriteBufferInv`, `NoDirtyReads` as invariants; additional run with `Serializable` as a PROPERTY):

| Invariant / Property | TLA+ Formula | TLC Result (2 threads) | TLC Result (1 thread) | Notes |
|----------------------|-------------|-----------------------|-----------------------|-------|
| `ClockParityInv` | `(clk % 2 = 1) ⇔ (∃ t : pc[t] = "committing")` | ✅ PASS | ✅ PASS | Strong invariant matches C++ CAS protocol |
| `CommitInvariant` | `∀ t : pc[t] = "committing" ⇒ readSet[t] = {}` | ✅ PASS | ✅ PASS | Read-set cleared before commit CAS; C++ commits always clear read-set |
| `WriteBufferInv` | `∀ t, a : a ∈ writeSet[t] ⇒ buffer contains a value` | ✅ PASS | ✅ PASS | Trivial property |
| `NoDirtyReads` | Reader with stale snapshot must be active (will abort) | ✅ PASS | ✅ PASS | Fundamental NOrec guarantee |
| `Serializable` (PROPERTY) | Committed transactions ordered by snapshot | ✅ PASS | ✅ PASS | Verified with extra TLC run using PROPERTY config (7M states) |
| Deadlock freedom | — | ✅ PASS | ✅ PASS | `-deadlock` flag; no deadlock found |

**TLC configuration summary:**
- `Thread = {1, 2}` (default config) or `{1}` (sequential); `Addr = {0, 1}`; `Data = {0}`
- 7,051,917 states generated / 796,501 distinct (2-thread), 1,173 / 283 (1-thread)
- Complete state-graph search, depth 32 (2-thread), 15 (1-thread)
- No error found across any configuration

---

## Deviations

Numbered list of abstraction gaps between the C++ implementation and the TLA+ model.

### 1. Clock double-check (torn-read protection) — NOT modeled

- **C++**: `NOrec.hpp:434–440` — `do { while(get_clock()&1) relax(); val=read(addr); } while(get_clock() != clock_before);`
- **TLA+**: No equivalent; memory reads are atomic at the level of a single TLA+ variable
- **Risk**: **Low**. This is a legitimate abstraction: TLA+ models memory as atomic values where a single variable assignment cannot be torn. The C++ clock double-check is an implementation adaptation to real hardware. The model's `ReadWithValidation` and `ReadFromMemory` rely on the same clock snapshot semantics. However, a derived property like "a reader never sees partially-applied writes" is assumed, not verified, in TLA+.

### 2. Plugin-mode bypass paths — NOT modeled

- **C++**: `NOrec.hpp:415–418` (read), `492–497` (write), `276–284` (commit write-back) — `#ifdef LLVM_TM_PLUGIN` guards bypass TM tracking for non-TM-region addresses
- **TLA+**: No concept of "plugin mode" or address-space qualifiers
- **Risk**: **High**. These bypasses are error-prone and have caused real bugs in other backends (see AGENTS.md — NOrec plugin bypass was identified 2026-06-23 as a correctness bug: `read_word_norec`/`write_word_norec` skip TM tracking for heap addresses not in the mmap region, making concurrent access unprotected). The model cannot catch this class of bug.

### 3. Type-interchange write-set logic — NOT modeled

- **C++**: `NOrec.hpp:337–404` — extensive write-set scan for POINTER↔UINT64 interchange, wider→narrower extraction, byte-merge
- **TLA+**: Single `Data` type per address; no type tagging or conversion
- **Risk**: **Low**. Type-interchange is a memory-model artifact of the LLVM instrumentation pass (different GEP expansions produce different value types for the same address). It does not affect the TM correctness properties (atomicity, serializability) as long as the final stored value is correct.

### 4. Memory ordering / fence tracking — NOT modeled

- **C++**: `NOrec.hpp` uses `memory_order_acquire` on `get_clock()` (line 150) and `memory_order_release` on `set_clock()` (line 153); CAS uses `compare_exchange_strong` with sequential consistency
- **TLA+**: No `lastFence[t]` tracking variable or `FenceFidelity` invariant
- **Risk**: **Medium**. Unlike the TinySTM backends (which received `lastFence` tracking in the 2026-06-23 session), NOrec's TLA+ model has zero fence modeling. On weakly-ordered architectures (ARM), missing acquire/release semantics could allow reordering that breaks the clock-based consistency. The C++ code correctly uses memory ordering, but the model does not verify it.

### 5. `rsSnapshot` redundant with `snapshot` — TLA+ abstraction

- **TLA+**: `rsSnapshot[t]` tracks the clock at last validation (line 47, set in `Begin` at line 86)
- **C++**: Only `tx->snapshot` exists; no separate `rsSnapshot`
- **Risk**: **None**. In the TLA+ model, `rsSnapshot` is set identically to `snapshot` in `Begin` and never referenced elsewhere. This is dead state in the model.

### 6. `lastWriter` / `lastWriteClock` — proof scaffolding, not implemented

- **TLA+**: `lastWriter[a]` tracks which thread last wrote to address `a`; `lastWriteClock[a]` tracks clock value when the write became visible (lines 48–49, updated in `CommitWriteBack`)
- **C++**: No equivalent tracking
- **Risk**: **None**. These are pure TLA+ proof artifacts used to state the `NoDirtyReads` and `Serializable` properties. They do not correspond to runtime data structures.

### 7. Abort mechanism: longjmp vs. state reset

- **C++**: `NOrec.hpp:223` — `siglongjmp(*jmpbuf, 1)` unwinds the execution stack
- **TLA+**: `ReadAbort` action resets `pc[t] := "idle"` and clears sets; control flow implicitly returns to `Begin` via `Next`
- **Risk**: **None**. The retry-loop semantic is the same: transaction restarts from the beginning with cleared state. The TLA+ model correctly captures the effect.

### 8. Read-set overflow check — NOT modeled

- **C++**: `NOrec.hpp:456–459` — fatal abort if read-set exceeds 1M entries
- **TLA+**: No size bounds on read-set
- **Risk**: **None**. This is a safety net against pathological instrumentation; not a correctness property.

### 9. Null/small-address safety bypass — NOT modeled

- **C++**: `NOrec.hpp:411–414` (read: return zero for addr < 0x100000), `488–491` (write: skip)
- **TLA+**: All addresses in `Addr` are valid
- **Risk**: **None**. This is a defense-in-depth guard against LLVM-generated invalid GEP addresses; orthogonal to TM correctness.

### 10. `Serializable` property not in default config

- **TLA+**: `Serializable` is defined (line 278) but NOT listed in `NOrec.cfg` (only `NoDirtyReads` is)
- **Finding**: Verified separately with a custom config — all 7M states pass. But the default CI run does not check it.
- **Risk**: **Low**. The property holds; it just isn't checked by default.

### 11. `THEOREM NOrecSerializability` not machine-checked

- **TLA+**: Line 288–289 — `THEOREM NOrecSerializability == Spec => ([]ClockParityInv /\ []NoDirtyReads)`
- **TLC**: Can verify that `Spec ⇒ []ClockParityInv` and `Spec ⇒ []NoDirtyReads` hold (checked as invariants), but cannot verify the THEOREM as a TLAPS proof
- **Risk**: **Low**. The inductive invariant proof is sketched (lines 218–228 for `ClockParityInductive`) but not machine-verified by TLAPS. TLC model-checking provides confidence for finite instances.

### 12. `Init` sets `wbBuffer[t][a] = 0` for all addresses — unused initial value

- **TLA+**: `wbBuffer` is initialized to all zeros (line 66)
- **C++**: No equivalent pre-initialization
- **Risk**: **None**. TLA+ places holder; the only meaningful `wbBuffer` values are for addresses in `writeSet[t]`.

---

## Summary

| Aspect | Verdict | Details |
|--------|---------|---------|
| **Algorithm fidelity** | ⚠️ Good | Core NOrec algorithm (begin/read/write/commit/validate) is faithfully captured. Commit CAS protocol is split into success/failure/retry actions matching C++ exactly. |
| **TLC verification** | ✅ Strong | 4 invariants + 2 properties verified across 7M states (2 threads). Both `NoDirtyReads` and `Serializable` pass. No deadlock. |
| **Plugin-mode modeling** | ❌ Missing | The `#ifdef LLVM_TM_PLUGIN` bypass paths (lines 415–418, 492–497, 276–284) have zero model coverage. These are known bug vectors. |
| **Fence / memory ordering** | ⚠️ Weak | No `lastFence` tracking; no `FenceFidelity` invariant. Contrast with TinySTM models which received these in 2026-06-23. |
| **Type system abstraction** | ✅ Appropriate | TLA+ abstract type `Data` is sufficient for correctness properties. The C++ type-interchange layer is an instrumentation artifact. |
| **Torn-read protection** | ⚠️ Abstracted | The clock double-check (lines 434–440) is not modeled because TLA+ assumes atomic memory. This is a reasonable abstraction but means the model does not verify the double-check's correctness. |
| **Lifecycle / allocators** | ⚠️ Partial | `real_tm_begin`/`end` wrappers and deferred-free / spec-alloc handling are orthogonal but the model tracks only begin/commit. The allocator interaction with TM region is not modeled. |
| **Config completeness** | ⚠️ Minor | `Serializable` property is declared but not in default `NOrec.cfg`. Verified separately — passes. |
| **TLAPS proof** | ⚠️ Partial | `ClockParityInductive` has a proof sketch but is not machine-verified. The main theorem `NOrecSerializability` depends on it. |

### Overall: **3/5**

The NOrec TLA+ model is a solid representation of the paper algorithm. The core commit-CAS protocol, read-set validation, and clock-parity invariants are verified. However, the model has not kept pace with C++ implementation changes: the plugin-mode bypass paths (a known correctness issue from the 2026-06-23 session) have zero coverage, fence tracking is absent (unlike the TinySTM models which were recently updated), and the torn-read double-check is abstracted away. The model would benefit from: (1) adding `lastFence` tracking + `FenceFidelity` invariant, (2) documenting the plugin bypass gap, and (3) adding `Serializable` to the default config.
