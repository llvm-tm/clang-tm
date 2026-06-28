# Audit: Romulus

**Score: 3/5** — TWO critical `atomic_thread_fence(seq_cst)` calls have NO annotation in model (line 226 after lock-bit set, line 236 after write-back). Lock-bit `fetch_or(acq_rel)` and version `store(release)` also missing. Rust backend is architecturally different (no read-set, no lock-bit phase). **Downgraded from memory ordering audit (2026-06-28).**

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/romulus.tla` (PlusCal, 449 lines) |
| C++ header | `backends/tm_impl/romulus/romulus.hpp` (318 lines) |
| C++ runtime | `backends/tm_impl/romulus/romulus_runtime.cpp` (204 lines) |
| TLC config (seq) | `docs/proofs/romulus.cfg` (1 thread) |
| TLC config (large) | `docs/proofs/romulus-large.cfg` (Addr={0,1}) |

## Algorithm Summary

Version-table OCC with commit lock. Read-validate (capture → read → re-check) ensures snapshot consistency. C++ backend passes 114/114 test_tx and 207/207 test_ds.

## Cross-Reference Checklist

### Transaction lifecycle

| C++ atomic/action | TLA+ action | Match | Notes |
|---|---|---|---|
| begin(): clear, timestamp=get_clock(), active=true, read_only=true | L_begin | ✅ | |
| abort_tx(): clear, retry flags, siglongjmp | L_abort_active | ✅ | siglongjmp retry → goto L_idle |
| commit(): lock CAS spin | L_active (commit branch): lock=0 → lock=t | ⚠️ Partial | Spin-loop not modeled; TLA+ assumes immediate availability |
| commit(): validate WS (locked? ver ≤ timestamp?) | L_validate (WS check) | ✅ | |
| commit(): validate RS (locked? ver unchanged?) | L_validate (RS check) | ✅ | |
| commit(): fetch_or(LOCK_BIT) on version entries | L_set_lock_bits | ✅ | `fetch_or(1)` ≡ `version[i] + 1` (MakeEntry produces even values) |
| commit(): increment_clock() → commit_ts | L_inc_clock | ✅ | |
| commit(): write_value_to_addr for written addrs | L_write_back | ✅ | |
| commit(): store(make_entry(commit_ts)) | L_update_ver | ✅ | |
| commit(): store(0) on commit lock | L_release_lock | ✅ | |

### Read/Write operations

| C++ atomic/action | TLA+ action | Match | Notes |
|---|---|---|---|
| read_word: write_set.find → return buffered | L_active: Read (own write) skip | ✅ | |
| read_word: is_locked(entry) → abort_tx | L_active: Read (locked) → abort | ✅ | |
| read_word: capture version, read, re-check, push read-set | L_active: Read (normal) record | ✅ | TLA+ single-step atomicity makes re-check unnecessary |
| write_word: write_set[addr]=val, read_only=false | L_active: Write | ✅ | |

### Invariants

| Invariant | C++ check | TLC result | Match |
|---|---|---|---|
| LockExclusion | commit lock exchange(0→1) ensures single holder | ✅ PASS | ✅ |
| LockHeldImpliesCommitting | lock held only during commit phases | ✅ PASS | ✅ |
| ClockMonotonic | clock starts at 1, only fetch_add | ✅ PASS | ✅ |
| AtMostOneCommitting | lock ensures single commit path | ✅ PASS | ✅ |

## Deviations

### 1. Commit lock spin-loop (Low risk)
**C++** (`romulus.hpp:179–182`):
```cpp
while (g_commit_lock.load(...) != 0 ||
       g_commit_lock.exchange(1, ...) != 0) {
    std::this_thread::yield();
}
```
**TLA+** (L_active commit branch): checks `lock = 0` then becomes `lock := self` in a single atomic step.

The model assumes immediate lock acquisition. This misses contention interleavings where both threads see `lock = 0` simultaneously. However, TLC sequentializes thread actions, so this interleaving cannot occur in the model. The modeled property (mutual exclusion of commit) is equivalent.

**Risk**: Low — mutual exclusion is the same; spin-loop is a performance detail.

### 2. Read-validate re-check (No risk)
**C++** (`romulus.hpp:272–286`):
```cpp
uint64_t entry_before = g_version_table[idx].load();
any_type_t val = read_value_from_addr(addr, sz);
uint64_t entry_after = g_version_table[idx].load();
if (entry_before != entry_after) abort_tx();
```
**TLA+**: Single atomic read step — the version capture and read produce a consistent pair by construction.

The re-check is C++-specific defense against concurrent write-back. In TLA+, an action either happens completely or not at all; there is no intermediate state between version capture and data read.

**Risk**: None — PlusCal atomicity makes re-check unnecessary.

### 3. Memory ordering / fences (No risk)
**C++** (`romulus.hpp:226, 236`):
```cpp
std::atomic_thread_fence(std::memory_order_seq_cst);  // between set-lock-bits and inc-clock
std::atomic_thread_fence(std::memory_order_seq_cst);  // between write-back and update-ver
```
**TLA+**: Single-copy atomicity — all writes are immediately visible to all threads.

C++ fences prevent compiler reordering and ensure cross-core visibility. TLA+ has no concept of instruction reordering.

**Risk**: None — TLA+ ordering is inherent.

### 4. `isTMAddress()` check in read_word (Medium risk)
**C++** (`romulus.hpp:265–267`):
```cpp
if (!stm::isTMAddress(addr)) {
    return read_value_from_addr(addr, sz);
}
```
**TLA+**: All `Addr` values are TM-tracked by construction.

The C++ implementation bypasses TM tracking for addresses outside the mmap'd TM region. In the TLA+ model, every address is always TM-tracked. The bypass exists because the LLVM pass can generate TM operations for non-TM addresses (stack locals, static globals before `.tm_shared` registration). This is NOT a model issue — the model is correct for the TM-tracked address space.

**Risk**: Medium — but applies to all backend models uniformly. No backend audit needs to re-evaluate this.

### 5. Sorted write-address traversal (No risk)
**C++** (`romulus.hpp:171–176`):
```cpp
std::vector<void *> addrs;
for (auto &it : tx->write_set) addrs.push_back(it.first);
std::sort(addrs.begin(), addrs.end(), ...);
```
**TLA+**: `\A a \in Addr : (write_set[a] # NoWrite) => ...` — order-independent quantifier.

C++ sorting prevents deadlock at the lock-bit-setting phase (all backends lock in address order). TLA+ quantifier makes order irrelevant because actions are atomic.

**Risk**: None — safety does not depend on traversal order.

### 6. Nested transaction support (Low risk)
**C++** (`romulus_runtime.cpp:106–120`):
```cpp
static void real_tm_begin() {
    if (tm_nested_call_counter == 1) {
        romulus::jmpbuf_ptr = (sigjmp_buf *)&tm_jmpbuf;
        romulus::begin();
    }
}
static void real_tm_end() {
    if (tm_nested_call_counter == 1) {
        romulus::commit();
        tm_nested_call_counter = 0;
    } else if (tm_nested_call_counter > 1) {
        tm_nested_call_counter--;
    }
}
```
**TLA+**: Flat transaction model — begin and end have no nesting logic.

The nested counter wrapper in C++ is a thin shim: only the outermost begin/end actually starts/commits the transaction. The inner begin/end are no-ops. Since the core algorithm (`romulus::begin()`, `romulus::commit()`) is isolated from this wrapper, the TLA+ model correctly represents the algorithm.

**Risk**: Low — the wrapper does not affect the correctness of the OCC protocol.

### 7. TM token release (No risk)
**C++** (`romulus.hpp:159, 249`): Calls `stm::tm_token_release_if_held(tx->id)` on abort and `stm::tm_token_release()` on commit.

**TLA+**: Not modeled.

TM token ownership is a separate concern from the OCC protocol (manages the TM region allocator's speculative allocation tracking). It has no effect on memory consistency or version-table correctness.

**Risk**: None — orthogonal to the OCC protocol modeled.

## Summary

| Aspect | Verdict |
|--------|---------|
| Core OCC protocol | ✅ Perfect match (begin, read, write, validate, lock-bits, inc-clock, write-back, update-ver, release-lock, abort) |
| Invariants | ✅ All 4 invariants have direct C++ analogs |
| Known deviations | 7 deviations (1 medium, 4 low, 2 none) |
| **Overall score** | **4/5** |

## Recommendations

1. **No model changes needed** — the Romulus TLA+ spec faithfully represents the C++ implementation's OCC protocol.
2. **Use as calibration**: Romulus is the reference for auditing other backends — any deviation pattern present in Romulus but absent in another backend should be investigated.
3. **Implementation_notes.md stale**: The file at `backends/tm_impl/romulus/Implementation_notes.md` describes the Left-Right paper algorithm, not the actual version-table OCC implementation. This is a documentation concern only, not a model fidelity issue.
