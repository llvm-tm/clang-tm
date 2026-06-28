# Audit: LEFTRIGHT (Global-Clock OCC / Value-Based Validation)

**Score: 3/5** — Write path has ZERO ordering operations in C++ but model annotates `"acq"` (wrong direction). Read-path data-race vulnerability on ARM (plain `read_value_from_addr` before `get_clock()` acquire-load — CPU can reorder). Queue-mode bypass not modeled. **Downgraded from memory ordering audit (2026-06-28).**

## Files

| Artifact | Path | Lines |
|----------|------|-------|
| TLA+ spec | `docs/proofs/LEFTRIGHT.tla` (PlusCal) | 329 |
| C++ header | `backends/tm_impl/leftright/leftright.hpp` | 339 |
| C++ runtime | `backends/tm_impl/leftright/leftright_runtime.cpp` | 234 |
| TLC config (seq) | `docs/proofs/LEFTRIGHT-sequential.cfg` | 9 |
| TLC config (par) | `docs/proofs/LEFTRIGHT.cfg` | 9 |
| TLC config (large) | `docs/proofs/LEFTRIGHT-large.cfg` | 9 |

## Algorithm Summary

Global-clock OCC with value-based validation. Writes are buffered in a write-set; the read-set records (address, observed_version, captured_value) triples. Commit: validate read-set (version check), acquire global commit lock, re-validate with value-based memcmp, increment clock, write-back, release lock. Despite its name, this is not classic Left-Right synchronization — it is a global-clock OCC with a serialized commit path.

## Cross-Reference Checklist

| C++ function | TLA+ label/action | Match | Notes |
|---|---|---|---|
| `begin()`: clear, `start_version=get_clock()`, `active=true`, `read_only=true` | `L_begin`: `snapshot := clock`, clear `read_set`/`write_set`, `read_only := TRUE` | ✅ | |
| `read_word()`: check write-set for own writes first | `L_active` Read: `IF write_set[a] # NoWrite THEN skip` | ✅ | |
| `read_word()`: `val = read_value_from_addr(addr)`, then `observed_version = get_clock()`, log as `<<addr, ver, val>>` | `L_active` Read: record `<<a, clock, mem[a]>>` | ⚠️ | **Order reversed**: C++ reads data BEFORE clock; TLA+ reads clock and data atomically. Safe (conservative — may cause false aborts) but not equivalent (see Deviation 1). |
| `write_word()`: `read_only=false`, buffer `WriteLogEntry` in `write_set` | `L_active` Write: `write_set[a] := v`, `read_only := FALSE` | ✅ | |
| Read-only commit: `if (read_only) return` | `L_active` read-only branch: `IF read_only THEN committed++ ; goto L_idle` | ✅ | |
| `validate()`: `∀r∈read_set: r.observed_version ≤ tx.end_version` | `L_validate`: `∀<<a,ver,val>>∈read_set: ver ≤ snapshot` | ⚠️ | **endVersion gap**: C++ checks against `tx.end_version` (can grow via `extend()`). TLA+ checks against `snapshot` (fixed at begin). See Deviation 8. |
| `commit()` Phase 1: optimistic validate BEFORE lock | _(not modeled)_ | ❌ | C++ validates outside lock as fast-path abort. TLA+ only validates after acquiring lock. Safe over-approximation (C++ does more checks). |
| `commit()` Phase 2: sort write-set (`std::sort` by address) | _(not modeled)_ | ❌ | Ordering irrelevant without per-address locks. Implementation detail. |
| `commit()` Phase 2: `acquire_commit_lock()` spin-loop | `L_active` acquire branch: `IF commit_lock=0 THEN commit_lock:=self` | ⚠️ | Spin-loop not modeled; atomic acquisition is safe over-approximation. |
| `commit()` Phase 3: re-validate under lock + value-based memcmp | `L_validate`: `ver ≤ snapshot ∧ mem[a] = val` | ✅ | TLA+ combines both version-check and value-check in one step (lock held, so no interleaving possible). |
| `commit()` Phase 4: `increment_clock()` | `L_inc_clock`: `clock := clock + 1` | ✅ | |
| `commit()` Phase 4: `write_value_to_addr` write-back | `L_write_back`: `mem[a] := write_set[a]` | ✅ | |
| `commit()` Phase 5: `release_commit_lock()` | `L_release_lock`: `commit_lock := 0`, `committed++` | ✅ | |
| `commit()` queue mode: validate+write-back without lock | _(not modeled)_ | ❌ | Queue mode skips commit lock entirely (relies on executor ordering). Not in TLA+. |
| `abort_tx()`: clear, `siglongjmp` retry | `L_abort`: clear sets, `aborted++`, goto L_idle | ⚠️ | `siglongjmp` non-local exit not modeled; TLA+ just loops to L_idle. Effect on state is same. Token release not modeled. |
| `isTMAddress(addr)` bypass | _(not modeled)_ | ❌ | Non-TM addresses bypass all TM tracking. TLA+ assumes all Addr are TM-tracked. |
| `read_word()`: non-queue-mode read-set logging only | `L_active` Read: always records | ⚠️ | TLA+ has no queue-mode branching; always records read-set. |
| `stm::tm_token_release()` / `tm_token_release_if_held()` | _(not modeled)_ | ❌ | Token management not in TLA+ model. |
| Memory ordering: `memory_order_acquire`, `memory_order_release`, `memory_order_acq_rel` | _(not modeled)_ | ❌ | **No fence annotations**. Unlike TinySTM models (which have `lastFence[t]`), LEFTRIGHT TLA+ has no `lastFence` variable or `FenceFidelity` invariant. |
| `extend()`: validate + update `end_version` | _(not modeled)_ | ❌ | No `L_extend` label, no per-thread `endVersion[t]`. |

## Invariants

All TLC runs use `Inv` (`LockExclusion ∧ LockHolderCommitting ∧ AtMostOneCommitting`) with `ModelBound` constraint.

| Invariant | TLC result | Notes |
|-----------|-----------|-------|
| `LockExclusion` | ✅ PASS (seq: 73 states; par: 778k states) | At most one thread holds `commit_lock`. |
| `LockHolderCommitting` | ✅ PASS (seq + par) | Lock holder is in `{L_validate, L_inc_clock, L_write_back, L_release_lock}`. |
| `AtMostOneCommitting` | ✅ PASS (seq + par) | No two threads simultaneously in commit phases. |
| `Inv` (combined) | ✅ PASS (seq + par) | |
| Large model (`Addr={0,1}`, `Thread={1,2}`, `MaxCommits=2`) | ⏱ Timed out (120s) | State explosion with 2 addresses. |

### TLC configuration

```
Sequential: Thread={1}, Addr={0}, Data={0,1}, MaxCommits=1 → 73 states, depth 12
Parallel:   Thread={1,2}, Addr={0}, Data={0,1}, MaxCommits=2 → 778,676 states, depth 60
Large:      Thread={1,2}, Addr={0,1}, Data={0,1}, MaxCommits=2 → timeout (>120s)
```

## Deviations

### 1. Read-clock ordering (Medium risk — conservative, not unsound)

**C++** (`leftright.hpp:285–296`): `read_word()` reads the data value FIRST:
```cpp
any_type_t val = read_value_from_addr(addr, sz);
// ... concurrent commit could write-back here ...
entry.observed_version = get_clock();
entry.captured_value = val;
```
A concurrent commit writes data and increments the global clock between the data read and the clock read. The result: `captured_value` is consistent with a state at some time `T_data`, but `observed_version` reflects `T_clock > T_data`. At validation, `observed_version > snapshot` fires and the transaction aborts — even though the data was read from a consistent intermediate state (no atomicity violation occurred).

**TLA+** (`LEFTRIGHT.tla:192–193`): reads clock and memory atomically:
```
read_set[self] ∪ {<<a, clock, mem[a]>>}
```

**Risk**: Medium. The C++ ordering is strictly more conservative — it can cause false aborts that the TLA+ model would not predict. This means a real execution may have a *higher* abort rate than the model implies. No correctness violation, but the model under-estimates abort frequency.

### 2. Pre-lock optimistic validate (Low risk)

**C++** (`leftright.hpp:212–216`): Before acquiring the commit lock, `commit()` calls `validate()` as a fast-path check. If the read-set is already stale, the transaction aborts without contending for the lock.

**TLA+**: Validation only occurs AFTER acquiring `commit_lock` (label `L_validate`). There is no pre-lock validate step.

**Risk**: Low. The pre-lock validate is a performance optimization (reduces lock contention from already-doomed transactions). It does not affect safety — any transaction that passes the pre-lock validate would still need to pass the under-lock validate. The TLA+ model over-approximates lock acquisition (transactions acquire the lock that C++ would have aborted earlier), which is safe for invariant checking.

### 3. Spin-loop lock acquisition (Low risk)

**C++** (`leftright.hpp:220–221`): A `while` loop with `std::this_thread::yield()` spins until `acquire_commit_lock()` succeeds:
```cpp
while (!acquire_commit_lock())
    std::this_thread::yield();
```

**TLA+** (`LEFTRIGHT.tla:210–211`): Atomically checks `commit_lock = 0` and sets `commit_lock := self` in a single step. No spinning.

**Risk**: Low. The spin-loop is an implementation detail of lock acquisition. The model's atomic acquisition is equivalent in terms of safety (the lock is still exclusive). In the TLA+ model, a thread that cannot acquire the lock simply stays in `L_active` and retries on the next non-deterministic step — same effect.

### 4. `siglongjmp` non-local exit (Low risk)

**C++** (`leftright.hpp:151`): `abort_tx()` calls `siglongjmp(*jmpbuf_ptr, 1)` to unwind the call stack and restart the transaction at the `sigsetjmp` point. The transaction state is cleared before the jump.

**TLA+** (`LEFTRIGHT.tla:248–254`): `L_abort` clears read-set, write-set, sets `snapshot := 0`, increments `aborted[t]`, and transitions to `L_idle`. No non-local control flow.

**Risk**: Low. The effect on transactional state is identical (sets cleared, retry). The `siglongjmp` mechanism is a runtime implementation concern. Token release (`stm::tm_token_release_if_held` at `leftright.hpp:149`) is also not modeled — but tokens are advisory (not a correctness mechanism).

### 5. Queue mode path (Low risk)

**C++** (`leftright.hpp:192–203`): When `isQueueActive()` is true, `commit()` skips the commit lock entirely and writes back directly after validation. Queue workers are serialized by the executor.

**TLA+**: No queue mode — every commit must acquire the commit lock.

**Risk**: Low. Queue mode serializes transactions at the executor level, making the lock redundant. This is an optimization, not a correctness change. The TLA+ model would still be correct if applied to queue mode (it would just serialize at the lock instead of the executor — same effect).

### 6. `isTMAddress` bypass (No risk)

**C++** (`leftright.hpp:282–283, 310–313`): `read_word()` and `write_word()` bypass TM tracking for addresses not in the TM mmap region, falling through to direct memory access.

**TLA+**: All addresses in `Addr` are TM-tracked; no bypass exists.

**Risk**: None. The bypass targets stack addresses and non-TM heap data (e.g. `TM<int*>::alloc()` on the regular heap). The TLA+ model assumes a uniform address space, which is the standard abstraction for protocol-level verification.

### 7. Token management (No risk)

**C++** (`leftright.hpp:149, 267`): `stm::tm_token_release_if_held(tx->id)` and `stm::tm_token_release()` manage lease tokens for the TM region allocator. These are memory-management concerns orthogonal to the OCC protocol.

**TLA+**: Not modeled. The model has no concept of region allocation or lease tokens.

**Risk**: None. Token management does not interact with the OCC commit protocol.

### 8. Missing `endVersion`/extend modeling (Medium risk — model is more conservative)

**C++** (`leftright.hpp:134, 164–169`): `begin()` sets `tx->end_version = tx->start_version`. The `extend()` function calls `validate()`, then sets `end_version = get_clock()`, widening the validation window. This allows long-lived transactions to accept higher observed versions without aborting.

**TLA+** (`LEFTRIGHT.tla:66–71`): `L_begin` sets `snapshot := clock`. The `snapshot` is never updated after begin. Validation checks `ver ≤ snapshot[self]` — a fixed window.

**Risk**: Medium. The TLA+ model is *more* conservative than C++: it aborts transactions whose read-set entries have versions between `snapshot` and `end_version` (after extend), while C++ would allow them. This means the model predicts more aborts than C++ would actually experience. It does NOT miss any correctness-violating interleavings (if a transaction is correct under the stricter model, it is correct under the looser C++ semantics). But it limits the model's usefulness for predicting real-world abort rates.

### 9. Missing fence/memory-ordering annotations (Medium risk)

**C++** (`leftright.hpp` passim): Uses `std::memory_order_acquire` (e.g. `get_clock()`), `std::memory_order_release` (e.g. `release_commit_lock()`), `std::memory_order_acq_rel` (e.g. `increment_clock()`), `std::memory_order_relaxed` (abort count). These are explicit ordering constraints on the atomic operations.

**TLA+**: All shared-variable reads and writes are atomic and sequentially consistent. No fence tracking variable exists in the model.

**Risk**: Medium. The TinySTM models (WBCTL, WBETL, WT) were recently updated with `lastFence[t]` tracking and `FenceFidelity` invariant (see `docs/AGENTS.md` 2026-06-23). The AGENTS.md plans to add `lastFence` to LEFTRIGHT (and other backends) in a subsequent session. Without fencing annotations, the model cannot detect missing memory barriers that could cause stale-read or reordering bugs at the hardware level. However, the C++ LEFTRIGHT backend uses correct acquire/release semantics on all shared variables, so there is likely no actual ordering bug — the model just cannot *prove* the absence of one.

### 10. Write-set sorted order (No risk)

**C++** (`leftright.hpp:206–210`): Sorts write-set addresses by `(uintptr_t)a` for deterministic write-back order. This is preparation for potential future per-address locking.

**TLA+** (`LEFTRIGHT.tla:234–236`): Writes back all addresses in `write_set[self]` via a set-comprehension map update. Order is not specified.

**Risk**: None. Write-back order is irrelevant when no per-address locks are held (the commit lock serializes the entire write-back).

### 11. Sub-word type merging (No risk)

**C++** (`leftright.hpp:37–48`): `ReadLogEntry` and `WriteLogEntry` store a `ValueType` enum to distinguish UINT8/16/32/64/FLOAT/DOUBLE/POINTER. The `any_type_t` union handles type-specific sizes.

**TLA+**: Single `Data` value per address. No type system.

**Risk**: None. Standard TLA+ abstraction over C++ type polymorphism. The protocol (which addresses are read/written, when) is type-independent.

## Summary

| Aspect | Verdict |
|--------|---------|
| Core commit protocol (validate, lock, inc clock, write-back, unlock) | ✅ Well modeled — all 5 phases have TLA+ labels |
| Value-based validation | ✅ Covered by `mem[a] = val` in `L_validate` (under lock, equivalent to C++) |
| Write-set buffering + own-write visibility | ✅ TLA+ checks `write_set[a] ≠ NoWrite` before reading from mem |
| Lock invariants (`LockExclusion`, `LockHolderCommitting`, `AtMostOneCommitting`) | ✅ All 3 defined and PASS TLC |
| Read-only fast-path commit | ✅ Modeled in `L_active` read-only branch |
| `siglongjmp` retry / lazy abort | ⚠️ Abstracted: TLA+ transitions to `L_idle` (same effect on state) |
| Queue mode (no lock) | ❌ Not modeled |
| Pre-lock optimistic validate | ❌ Not modeled (performance optimization) |
| `extend()` / `endVersion` | ❌ Not modeled — `snapshot` is fixed at begin time |
| Memory ordering / fences (`lastFence`) | ❌ **Not modeled** — pending per AGENTS.md plan |
| Read-clock ordering (data before clock) | ⚠️ Reversed: C++ reads data before clock; TLA+ reads atomically |
| `isTMAddress` bypass | ❌ Not modeled (standard TM address space assumption) |
| Token management (lease release) | ❌ Not modeled (orthogonal to OCC protocol) |
| Write-set sort | ❌ Not modeled (no correctness impact) |
| Type merging / sub-word values | ❌ Not modeled (standard abstraction) |
| **Overall score** | **3/5** |

### Recommended model improvements

1. **Add `lastFence[t]` + `FenceFidelity` invariant** — following the TinySTM model pattern from the 2026-06-23 session. Track `lastFence[t]` across read, lock-acquire, clock-inc, validate, and unlock points. This enables TLC to verify that fences are present before all shared-memory operations.

2. **Add `endVersion[t]` and `L_extend` label** — model per-thread version extension. `L_extend` validates and sets `endVersion[t] := clock`. Update validation checks to use `endVersion[t]` (or `snapshot[t]` if not extended). This would fix Deviation 8.

3. **Add pre-lock validate step** — add an `L_validateOpt` label between `L_active` (acquire branch) and `L_validate`, or add an optimistic-validate non-deterministic choice in `L_active`. This would close Deviation 2.

4. **Fix read-clock ordering in model** — if the intent is to match C++'s data-before-clock semantics, the TLA+ Read action should read `mem[a]` before reading `clock`. This is a minor adjustment that would close Deviation 1.
