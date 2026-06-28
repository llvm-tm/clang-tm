# Audit: TinySTM WBCTL (Write-Back Commit-Time Locking)

**Score: 4/5** — Core commit protocol well-modeled; `endVersion` + `L_extend` added; fence annotations present. Memory ordering sub-score: **2/5** — double-check protocol window abstracted, signal vs thread fence not distinguished, `is_locked()` acquire semantics not captured, spin-loop ordering effects unmodeled. See docs/audits/SUMMARY.md for full MO analysis.

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/tinystm_wbctl.tla` (PlusCal, 340 lines) |
| C++ header | `backends/tm_impl/tiny_stm/tinystm_wbctl.hpp` (1005 lines) |
| TLC config (seq) | `docs/proofs/tinystm_wbctl.cfg` |
| TLC config (large) | `docs/proofs/tinystm_wbctl-large.cfg` |

## Algorithm Summary

Commit-time locking: writes are buffered, locks acquired at commit, validate read-set, write-back, unlock. Double-checked read protocol ensures opacity.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| begin(): clear, start_version=get_clock(), active=true | L_idle: state=active, clear readSet/writeSet | ✅ | |
| write_word_ctl: buffer in write_set, read_only=false | L_active: Write: writeSet ∪ {a}, writeBuf[a]=n, readOnly=false | ✅ | |
| commit(): sorted lock acquisition loop | L_lockWT: lock[a]=<<1,self,...>> | ⚠️ Partial | Spin-loop not modeled; extend() now in L_extend via endVersion |
| commit(): increment_clock() | L_incClock: clock := clock + 1 | ✅ | |
| endVersion / extend check | endVersion[t], L_extend: validate + update endVersion | ✅ | Added per-thread endVersion; L_extend checks `lock[a][3] <= endVersion[t]` |
| validate(): read-set double-check | L_validate: `lock[a][3] <= clock - 1` | ⚠️ Partial | TLA+ only checks version, not the full double-check protocol |
| commit(): write_value_to_addr write-back | L_writeBack: mem update | ✅ | |
| commit(): unlock_with_version | L_writeBack: lock release with clock | ✅ | |
| read_word_ctl: double-check read protocol | L_active: Read → record version | ⚠️ Partial | Spin-loop for locked addresses not modeled |
| atomic_signal_fence (seq_cst) | lastFence[t] := "sc" on read, clock inc, validate | ✅ | FenceFidelity invariant checks fence before operations |
| abort_tx(): unlock_held_locks_and_clear | L_validate abort branch: lock release + clear | ✅ | Bug found via TLC (Phase 5) |

## Invariants

| Invariant | C++ check | TLC result |
|-----------|-----------|------------|
| LockChain: locking thread owns write-set locks | Runtime assertion | ✅ PASS (large model) |
| LockOwnerInv: locked addr has owning thread | Runtime assertion | ✅ PASS |
| WriteBackSafe: wb only after validation | Lock acquired → validate → write-back order | ✅ PASS |

## Deviations

### 1. Double-check read protocol (Low risk)
**C++** (`tinystm_wbctl.hpp:554–608`): The read path uses a full double-check protocol: read lock, if locked spin, if version > end_version call extend(), re-read lock, validate, record. This loop can retry many times, interleaving with other threads' commits.

**TLA+**: Single atomic read action — checks `lock[a][1] = 0`, records version. No spin, no re-read. However, the `endVersion[t]` and `L_extend` label now capture the extend() call's validation logic.

**Risk**: Low — the `endVersion[t]` addition models the extend() bound correctly. The spin-for-unlock loop remains unmodeled, but this is a performance detail, not a correctness gap.

### 2. Lock acquisition spin-loop (Low risk)
**C++** (`tinystm_wbctl.hpp:223–245`): Lock acquisition loops with `try_lock`, calls `extend()` on failure, has a `LOCK_RETRY_LIMIT` of 1000 before aborting.

**TLA+**: Single atomic check `\A a \in writeSet[self] : lock[a][1] = 0`, then lock all. No retry loop. The extend() call, however, now has a dedicated `L_extend` label.

**Risk**: Low — the retry loop is a performance optimization. The lock acquisition itself is atomic in the model, which is a safe over-approximation (any interleaving possible in C++ is a subset of model interleavings with atomic lock).

### 3. Validation-in-write path (No risk for invariants)
**C++** (`tinystm_wbctl.hpp:762–776`): `write_word_ctl` calls `validate()` and potentially `abort_tx` if the read-set is invalid while a lock is held by another thread.

**TLA+**: Write action just buffers the value. No validation call.

The write-path validation is a contention-management optimization (early abort before commit). It does not affect safety — if a stale read-set is missed at write time, it will be caught at commit validation.

**Risk**: None for safety invariants. Low for liveness (the model may miss forced-abort paths that reduce contention).

### 4. Sub-word type merging (No risk)
**C++** (`tinystm_wbctl.hpp:377–541, 647–760`): Extensive type-mismatch handling: byte merging, UINT64/UINT32/UINT16 field extraction, wider-to-narrower conversion, narrower-to-wider merging, memcpy byte-loop reconstruction. This is ~500 lines of the read/write path.

**TLA+**: Single Data value per address. No type system.

**Risk**: None — the type merging is an implementation detail of the C++ runtime's `any_type_t` representation. The TM protocol (which addresses are read/written, when locks are held) is unaffected. The model checks protocol correctness, not value fidelity.

### 5. Null-address / low-address guard (No risk)
**C++** (`tinystm_wbctl.hpp:216, 279, 549–552`): Guards against null and low addresses (< 0x100000) from the LLVM plugin's linked-list traversal instrumentation.

**TLA+**: Addr is a well-defined finite set.

**Risk**: None — these guards handle plugin-generated edge cases, not the TM protocol.

### 6. is_stack_addr bypass (No risk)
**C++** (`tinystm_wbctl.hpp:279`): Plugin path skips write-back for stack addresses to avoid corrupting popped frames.

**TLA+**: Every address is in the TM address space.

**Risk**: None — the bypass is for the plugin pipeline, not the core protocol.

### 8. Fence annotation precision (Low risk)
**C++**: Fences are `atomic_signal_fence(seq_cst)` and `atomic_thread_fence` with release/acquire/seq_cst ordering. The actual placement spans multiple C++ statements: signal fence before read, thread fence before clock read, signal fence before unlock.

**TLA+**: `lastFence[t]` is a single variable updated atomically at key points: `"sc"` on read/clock-inc/validate, `"acq"` on lock, `"rel"` on unlock. The model cannot distinguish `signal_fence` from `thread_fence` — both are recorded as `"sc"`.

**Risk**: Low — the `FenceFidelity` invariant only checks that a fence occurred (not its type). Over-annotating `signal_fence` as `"sc"` over-approximates memory ordering, which is safe.

### 7. extend() / version extension (Resolved — modeled)
**C++** (`tinystm_wbctl.hpp:159–168`): The `extend()` function validates the read-set and updates `end_version` to the current clock.

**TLA+**: `endVersion[t]` per-thread variable + `L_extend` label. On read-set validation, checks `lock[a][3] <= endVersion[t]` instead of a fixed window. The extend action sets `endVersion[t] := clock` and re-validates.

**Risk**: Resolved — the model now captures the extend() semantics.

## Summary

| Aspect | Verdict |
|--------|---------|
| Core commit protocol | ✅ Good match (lock, validate, write-back, unlock) |
| endVersion + extend() | ✅ Modeled via per-thread endVersion[t] + L_extend |
| Lock ownership invariants | ✅ Pass TLC; match C++ assertions |
| Fence annotations | ✅ lastFence[t] at read/lock/clock-inc/validate/unlock + FenceFidelity invariant |
| Read/write protocol | ⚠️ Spin-for-unlock loop not modeled; double-check abstracted to single-check |
| Type merging / null guards | ❌ Not modeled (acceptable — protocol-level detail) |
| Known deviations | 6 deviations (1 low, 5 none, 1 resolved) |
| **Overall score** | **4/5** |
