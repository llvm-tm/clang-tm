# Audit: TinySTM WBETL (Write-Back Encounter-Time Locking)

**Score: 4/5** — Core protocol well-captured; commit split into 3 labels; `endVersion` + `L_extend` added; extend abort lock leak fixed; fence annotations track memory ordering.

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/tinystm_wbetl.tla` (PlusCal, 360 lines) |
| C++ header | `backends/tm_impl/tiny_stm/tinystm_wbetl.hpp` (442 lines) |
| TLC config | `docs/proofs/tinystm_wbetl.cfg` |

## Algorithm Summary

Encounter-time locking: locks acquired on first write, read-set validates at commit with incremental clock, bitmapped write-back. Key difference from WBCTL: locks held during the body of the transaction, providing early write-write conflict detection.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| begin(): clear, start_version=get_clock() | L_idle: state=active, clear readSet/writeSet | ✅ | |
| write_word_etl: try_lock → buffer | L_active: New write: lock[a] := <<1,self,...>>, writeBuf[a]=n | ✅ | |
| write_word_etl: update (lock already held) | L_active: Update write: buffer update, lock already held | ✅ | |
| write_word_etl: lock contention → abort | L_active: Write conflict abort: release all locks | ✅ | Bug found via TLC (Phase 5) |
| commit(): increment_clock | L_incClock: clock := clock + 1 | ✅ | Split from monolithic commit |
| commit(): validate read-set | L_validateETL: check lock[a][3] <= clock-1 | ✅ | Split from monolithic commit |
| commit(): write-back + unlock | L_writeBackETL: mem update + lock release | ✅ | Split from monolithic commit |
| endVersion / extend check | endVersion[t], L_extend: validate + update endVersion | ✅ | Added per-thread endVersion |
| read_word_etl: double-check protocol | L_active: Read: record version if not locked | ⚠️ Partial | Spin loop + re-read not modeled |
| atomic_signal_fence (seq_cst) | lastFence[t] := "sc" on read/clock-inc/validate | ✅ | FenceFidelity invariant |
| commit(): bitmap write-back | L_writeBackETL: `mem[a] := writeBuf[self,a]` | ⚠️ Partial | Bitmap granularity not modeled |

## Invariants

| Invariant | C++ check | TLC result |
|-----------|-----------|------------|
| MutexLocks: no two threads hold same lock | lock try_lock CAS ensures | ✅ PASS |
| LockOwnerTx: locked addr has owner in write-set | Runtime check in unlock | ✅ PASS |
| NoLocksAfterCommit: thread releases all locks | unlock_held_locks_and_clear | ✅ PASS |

## Deviations

### 1. Lock acquisition retry logic (Medium risk)
**C++** (`tinystm_wbetl.hpp:367–404`): The `write_word_etl` lock acquisition has a multi-phase retry: check `is_locked`, check `get_owner`, try `try_lock`, if fail call `validate()` and retry, use `tm_token_soft_spin` for backoff, abort as last resort.

**TLA+** (L_active New write): Single check `lock[a][1] = 0` → acquire immediately. No retry, no spin, no validate-during-write.

**Risk**: Medium — the C++ lock-acquisition path has significantly more interleavings than the model captures. The MutexLocks invariant (never two holders) is preserved by CAS, but the model may miss states where a thread holds a lock while its read-set is stale.

### 2. Bitmap write-back (No risk)
**C++** (`tinystm_wbetl.hpp:215–229`): Write-back iterates by aligned-8-byte address, writing only the valid bitmap bytes within each word.
**TLA+**: `mem[a] := writeBuf[self,a]` for each a. No bitmap granularity.

**Risk**: None — the bitmap is a storage optimization; the protocol effect (all written addresses are updated atomically at commit) is identical.

### 3. Double-check read protocol (Low risk)
Same as WBCTL deviation #1 — spin-loop for locked addresses not modeled.

### 4. Commit split into 3 labels (Resolved — modeled)
**C++**: Commit has distinct phases: increment clock, validate read-set, write-back, release locks.
**TLA+**: Now split into `L_incClock` + `L_validateETL` + `L_writeBackETL` — three separate labels that can interleave with other threads.

**Risk**: Resolved — the model now captures interleavings between commit phases.

### 5. Missing `extend()` during read/write (Resolved — modeled)
Same as WBCTL deviation #7 — `endVersion[t]` + `L_extend` now model the extend() semantics.

### 6. Extend abort lock leak (Resolved — fixed)
**TLC result**: The model revealed that the L_extend failure path went directly to `"idle"` without releasing write-set locks, causing LockOwnerInv violations.
**Fix**: Added `lock[a] := <<0, 0, lock[a][3]>>` before transitioning to `"idle"`.

**Risk**: Resolved — TLC verified the fix produces no invariant violations.

### 7. Fence annotation precision (Low risk)
Same as WBCTL deviation #8 — `signal_fence` and `thread_fence` both recorded as `"sc"`; `FenceFidelity` checks fence count, not type.

## Summary

| Aspect | Verdict |
|--------|---------|
| Encounter-time locking | ✅ Well-captured |
| Lock ownership invariants | ✅ All pass TLC |
| Write conflict abort release | ✅ Bug found via TLC |
| Commit phases (clock/validate/write-back) | ✅ Split into 3 labels |
| endVersion + extend() | ✅ Modeled via endVersion[t] + L_extend |
| Fence annotations | ✅ lastFence[t] + FenceFidelity |
| Lock retry / spin logic | ❌ Not modeled |
| Bitmap / type merging | ❌ Not modeled |
| **Overall score** | **4/5** |
