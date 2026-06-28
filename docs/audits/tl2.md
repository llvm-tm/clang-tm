# Audit: TL2 (Transactional Locking II)

**Score: 3/5** — Core commit protocol captured, but clock increment ordering is fundamentally wrong: C++ uses `fetch_add(relaxed)`, model annotates `"sc"`. Rust backend has architecturally different global commit lock + `fence(SeqCst)` at 6 points not present in C++ or model. Guard-table aliasing gap remains. **Downgraded from memory ordering audit (2026-06-28).**

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/TL2.tla` (PlusCal, 329 lines) |
| C++ header | `backends/tm_impl/tl2/tl2.hpp` (753 lines) |
| C++ runtime | `backends/tm_impl/tl2/tl2_runtime.cpp` (295 lines) |
| TLC config (parallel) | `docs/proofs/TL2.cfg` (Thread={1,2}, Addr={0}) |
| TLC config (sequential) | `docs/proofs/TL2-sequential.cfg` (Thread={1}, Addr={0}) |
| Implementation notes | `backends/tm_impl/tl2/Implementation_notes.md` (paper summary) |

## Algorithm Summary

TL2 is a commit-time locking STM with a global version clock and per-stripe versioned write-locks. Transactions snapshot the clock at begin, buffer writes in a write-set, acquire write-set locks at commit time, increment the clock, validate the read-set against guard versions, write back, and release locks with the new version. Read-only transactions commit without validation.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| `STM::begin()` — `start_version = get_clock()`, clear write_set/read_set, active=true | L_idle: `snapshot := clock`, clear readSet/writeSet, state="active" | ✅ | |
| `STM::read_impl()` — bloom filter check → write-set scan → guard load → record version in read-set → return value | L_active: ReadMiss — `if a notin writeSet`: record `<<a, GuardVersion(guard[a])>>` in readSet | ✅ | C++ records observed_version from guard; TLA+ records GuardVersion(guard[a]); both capture version once before reading value (no re-check) |
| `STM::write_impl()` — buffer value in write_set, update on re-write, set bloom filter | L_active: Write — `writeSet ∪ {a}`, `writeBuf[a] := n`, readOnly=FALSE | ⚠️ Partial | No bloom filter in model; C++ deduplicates overlapping writes by reusing WriteSetEntry at same address (TLA+ just union-adds, but overwrites writeBuf) |
| Read-only commit: `write_set.empty()` → return true immediately | L_active: Commit read-only — if readOnly then increment committed, go idle | ✅ | |
| **Lock acquisition**: `try_acquire_guard()` CAS loop per guard; `held_guard[]` dedups aliases; releases all on failure | L_active: Commit Phase 1 — check `∀a: GuardLocked(guard[a])=0` then atomically lock all write-set guards | ⚠️ Partial | TLA+ locks all guards atomically; C++ increments one-by-one with CAS, can partially acquire then release on failure |
| `increment_clock()` — `fetch_add(1, relaxed) + 1` | L_incClock: `clock := clock + 1` | ✅ | |
| **Validation**: `read_guard()` → check `LOCK_MASK && !our_lock` **OR** `current_version != observed_version` | L_validate: `∀<<a,v>>: GuardVersion(guard[a]) = v` | ⚠️ Partial | **C++ checks both lock-bit and version; TLA+ checks version only.** Missing lock-bit check means a concurrent writer's guard (locked but same version field) passes validation in TLA+ |
| Write-back: write all values to memory while holding guards | L_writeBack: `mem[a] := writeBuf[self, a]` for a in writeSet | ✅ | |
| Guard release: `store((commit_version << 1), release)` per unique guard | L_release: `guard[a] := MakeGuard(0, clock)` for a in writeSet | ✅ | |
| `abort_tx()` — release all write-set guards, siglongjmp | L_validate abort branch: release guards, clear readSet/writeSet, go idle | ⚠️ Partial | TLA+ abort is folded into validation failure only; C++ has independent abort_tx callable at any time (e.g., lock-acquisition failure) |
| `get_guard_idx()` — hash `(addr>>3) ^ (addr>>48) & (GUARD_TABLE_SIZE-1)` | `guard[a]` — one guard per address | ❌ | **Guard-table hashing with aliasing is a central TL2 feature not modeled.** C++: 8192 guards mapped from address space via XOR hash; TLA+: per-address guards with no aliasing |
| Bloom filter (`bloom_set`, `bloom_might_contain`) | not present | ❌ | Optimization only; no correctness impact |
| Type-interchange: wider/narrower reads, byte-merge, sub-offset extraction | not present | ❌ | Protocol detail; TLA+ has single `Data` value per address |
| `LLVM_TM_PLUGIN` bypass: non-TM address → direct load/store | not present | ❌ | Plugin pipeline detail; TLA+ addresses are always TM-tracked |
| `atomic_signal_fence(seq_cst)` before write; acquire/release on guards; relaxed on clock | not present | ❌ | No `lastFence` tracking (unlike TinySTM audits which now have `FenceFidelity`) |

## Invariants

| Invariant | TLA+ label | Checked by TLC? | Meaning | Result |
|-----------|-----------|-----------------|---------|--------|
| `LockConsistent` | Line 289-293 | ✅ (via `Inv`) | A guard is locked iff a thread in committing state has it in its write-set | ✅ PASS (Addr={0}, Thread={1,2}) |
| `SnapshotInv` | Line 305 | ✅ (via `Inv`) | Every thread's snapshot ≤ global clock | ✅ PASS |
| `NoDirtyRead` | Line 296-301 | ❌ (defined but NOT in `Inv`) | No thread reads a guard locked by another thread | Modeled but not checked — related to lock-bit validation gap |
| `Inv` | Line 308-310 | ✅ | `LockConsistent /\ SnapshotInv` | ✅ PASS |

**Note:** `NoDirtyRead` is defined at line 296 but omitted from `Inv` (line 308). This invariant would catch the lock-bit validation gap — it checks that no thread holds a read-set entry for an address whose guard is locked by a different (non-idle) thread. Its omission from TLC checking means the model does not verify that readers avoid locked addresses.

## Deviations

### 1. Lock-bit check in validation (Medium risk)

**C++** (`tl2.hpp:590-618`): Validation checks both `(current_guard & LOCK_MASK && !our_lock)` and `current_version != re.observed_version`. The lock-bit check catches the case where a concurrent writer holds the guard between lock-acquisition and write-back but has not yet released it with a new version. Without this check, a thread can commit with a stale read-set entry for an address currently being written by another transaction.

**TLA+** (`L_validate`, line 108): Checks only `GuardVersion(guard[a]) = v`. Since the guard version field does not change during lock acquisition (only the lock bit is set), a concurrent writer between lock and release will have the same version field as when the reader first observed it. Validation passes even though another thread is actively writing.

**Example trace:**
  1. Thread A reads addr X at version V. Guard: unlocked, version V.
  2. Thread B writes X: acquires guard (locked, version V), increments clock to V+1.
  3. Thread A commits: locks its own write-set Y, increments clock to V+2, validates read-set {<<X,V>>}.
  4. Guard[X] is locked, version V. C++: abort (lock bit set, not our lock). TLA+: pass (V == V).
  5. Thread A writes Y and commits. Decision was based on stale read of X (B hasn't written X yet).
  6. B writes X, releases guard (version V+1). Serializability violated.

**Risk:** Medium — the model allows serializability violations that the C++ prevents. The TLC-checked invariants (`LockConsistent`, `SnapshotInv`) still hold in these scenarios, but the model is not faithful to C++'s validation strength. The defined but unchecked `NoDirtyRead` invariant directly captures this property.

### 2. Guard-table aliasing (Medium risk)

**C++** (`tl2.hpp:190-197`): TL2 uses per-stripe (PS) locking: `g_guards[GUARD_TABLE_SIZE]` with 8192 entries. Multiple addresses map to the same guard via `((addr>>3) ^ (addr>>48)) & mask`. The commit path explicitly handles aliasing via `held_guard[]` (line 563): acquires/releases each guard at most once regardless of how many write-set entries map to it. Comments at lines 562 and 620-624 explain the correctness rationale.

**TLA+** (lines 41-42): One guard per address — no aliasing. `guard[a]` is indexed directly by address.

**Risk:** Medium — guard aliasing is a fundamental design parameter of TL2's per-stripe scheme. The model cannot detect:
- False conflicts from hash collisions (two addresses sharing a guard without sharing data)
- The correctness-critical publish-safety pattern where all write-set entries sharing a guard must be written to memory BEFORE that guard is released with the new version (C++ writes all values first, then releases unique guards once)

The publish-safety concern (releasing a guard before all aliased addresses are written) IS correctly implemented in both C++ and the model: C++'s two-phase release (all writes, then all releases) matches TLA+'s L_writeBack followed by L_release. The model handles this correctly because there is no aliasing — each address has its own guard. But the model's structural abstraction means aliasing-induced behavior (false conflicts, dedup bookkeeping) is not validated.

### 3. Fence tracking absent (Medium risk)

**C++** (`tl2.hpp:227-285`): Memory ordering annotations throughout:
- `atomic_signal_fence(seq_cst)` before write (line 328)
- `memory_order_acquire` on guard loads (lines 237, 242, 258, 264, 286)
- `memory_order_release` on guard stores (lines 277, 282)
- `memory_order_relaxed` on clock operations (lines 228, 232)
- `memory_order_seq_cst` on init (line 218)

**TLA+**: No memory ordering annotations. No `lastFence` tracking.

**Risk:** Medium — the TinySTM WBCTL/WBETL/WT models were recently upgraded (2026-06-23) with `lastFence[t]` tracking and `FenceFidelity` invariant. TL2 lacks equivalent treatment. While TL2 has a simpler read/commit protocol than TinySTM (no extend(), no double-check read), the fence annotations are still critical for correct behavior on weakly-ordered architectures (ARM, Power). The model as-is does not validate that fences occur at the correct protocol points.

### 4. Atomic lock acquisition vs incremental CAS loop (Low risk)

**C++** (`tl2.hpp:562-581`): Locks are acquired one at a time via `try_acquire_guard()` which uses a CAS loop. If any lock acquisition fails (guard already held by another thread), all previously acquired locks are released and the transaction aborts. Partial acquisition is a real transient state.

**TLA+** (`L_active`, Commit Phase 1 lines 90-101): Checks `∀a ∈ writeSet: GuardLocked(guard[a]) = 0`, then atomically locks all write-set addresses in a single transition. No partial acquisition state.

**Risk:** Low — this is a safe over-approximation. Any interleaving reachable in the model (atomic lock) is reachable in C++ (incremental lock). The reverse is not true: C++ has transient states (some locks held, others not) that the model doesn't explore. However, these transient states are only observable by concurrent threads through the guard values, and the model's invariants don't depend on the exact number of locks held at a given instant.

### 5. Bloom filter (No risk)

**C++** (`tl2.hpp:199-214, 414-415`): 64-bit Bloom filter for fast negative write-set lookup. On read, if the Bloom filter doesn't contain the address, the write-set scan is skipped. On write, bits are set for all 8 byte offsets.

**TLA+** (lines 69-71 of ReadMiss): Write-set membership is checked directly via `a \notin writeSet[self]`.

**Risk:** None — the Bloom filter is a performance optimization with no correctness impact. The `bloom_might_contain` function can produce false positives (triggering a write-set scan) but never false negatives. Correctness does not depend on it.

### 6. Type merging and sub-word operations (No risk)

**C++** (`tl2.hpp:421-502`): ~80 lines of type-interchange logic: wider-to-narrower extraction, narrower-to-wider byte merging, sub-offset extraction from wider entries, same-size pointer/value interchange.

**TLA+**: Single value per address. No type system.

**Risk:** None — type merging is an implementation detail of C++'s byte-granularity representation. The TM protocol (which addresses are read/written, when locks are held) is unaffected. The model checks protocol correctness, not value fidelity.

### 7. LLVM_TM_PLUGIN address bypass (No risk)

**C++** (`tl2.hpp:331-338, 402-412`): When compiled with `LLVM_TM_PLUGIN`, addresses outside the TM region bypass all TM tracking and do direct loads/stores.

**TLA+**: Every address is in the TM address space.

**Risk:** None — this is a plugin pipeline integration detail, not part of the TM protocol.

### 8. Deferred frees and spec allocs (No risk)

**C++** (`tl2_runtime.cpp:123-143`): `real_tm_begin` clears deferred frees and spec allocs; `real_tm_end` flushes them. These are memory management mechanisms for supporting in-transaction `free()` and speculative allocations.

**TLA+**: Not modeled.

**Risk:** None — memory management is outside the TM protocol scope.

### 9. NoDirtyRead invariant omitted from TLC checking (Low risk)

**TLA+** (`TL2.tla:296-301, 308-310`): `NoDirtyRead` is defined (checks no thread reads a locked guard held by another) but deliberately excluded from `Inv`. The comment at line 303-310 shows `Inv` only includes `LockConsistent` and `SnapshotInv`.

**C++**: The lock-bit check in commit validation (gap #1 above) implicitly enforces `NoDirtyRead` by aborting when a read-set guard is locked by another thread.

**Risk:** Low — the invariant is defined in the spec as documentation but not verified by TLC. If it were checked, the model would likely fail (it allows the validation-lock-bit gap scenario). Adding it to `Inv` would force a model correction (add lock-bit check to L_validate).

## Summary

| Aspect | Verdict |
|--------|---------|
| Core commit protocol (begin, read, write, lock, inc clock, validate, write-back, release) | ✅ Well-captured; phases match C++ order exactly |
| Read-only commit optimization | ✅ Aligned — both skip lock/validate/write-back |
| Read path (version capture without re-check) | ✅ Aligned — neither C++ nor TLA+ does double-check read (unlike TinySTM WBCTL) |
| Guard-table hashing and aliasing | ❌ **Central TL2 design feature abstracted** — per-address guards lose alias semantics |
| Validation lock-bit guard | ❌ C++ checks lock-bit + version; TLA+ checks version only — **allows stale-commit scenarios** |
| Fence tracking | ❌ Not present (unlike TinySTM audits which have `lastFence` + `FenceFidelity`) |
| Lock acquisition | ⚠️ Atomic in model, incremental CAS in C++ |
| Bloom filter | ❌ Not modeled (performance only) |
| Type merging / LLVM bypass / memory mgmt | ❌ Not modeled (acceptable — protocol-level detail) |
| Known deviations | 8 deviations (2 medium, 2 low, 4 none) |
| **Overall score** | **3/5** — Core protocol captured but two significant gaps (lock-bit validation, guard aliasing) and no fence tracking keep it below TinySTM's 4/5 |
