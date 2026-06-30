# Audit: XTM — Page-Granularity OCC with Private Copies

**Score: 4/5** — `lastFence` split into `lastSignalFence`/`lastThreadFence`/`lastRmw` (3-variable model). Read path correctly uses `lastRmw="acquire"` (`load(acquire)`). CAS page acquisition uses `lastRmw="acq_rel"` (`compare_exchange_strong(acq_rel)`). Commit version bump uses `lastRmw="acq_rel"` (`fetch_add(acq_rel)`). Validate success uses `lastRmw="acquire"`. XTM uses zero fences — all ordering via RMW and acquire/release loads/stores, now correctly reflected. Bloom filter absent. Rust backend different algorithm (version-table OCC). **Updated 2026-06-30.**

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/XTM.tla` (PlusCal, 358 lines) |
| C++ header | `backends/tm_impl/xtm/xtm.hpp` (347 lines) |
| TLC config (seq) | `docs/proofs/XTM-sequential.cfg` |
| TLC config (2-thread) | `docs/proofs/XTM.cfg` |
| TLC config (large) | `docs/proofs/XTM-large.cfg` |

## Algorithm Summary

Page-granularity OCC with private copies: on first write to a page, CAS-acquire ownership via the XADT hash table and create a `memcpy`'d private copy; subsequent writes go to the private copy. Reads snapshot the page version, abort immediately if the page is owned by another transaction, and read from the private copy if owned by self. Commit validates all read-set versions, then write-backs dirty pages, bumps page versions, and releases ownership.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| `begin()`: clear, active=true, read_only=true | `L_begin`: read_set={}, write_set=NoWrite, goto L_active | ✅ | |
| `read_word`: isTMAddress bypass | Not modeled | ❌ | Defense-in-depth only |
| `read_word`: owner check → abort if owned | L_active: Read (conflict) — `xadt_owner[p] \notin {0, self}` → abort | ✅ | |
| `read_word`: write-set lookup | L_active: Read own write — `write_set[p] # NoWrite` → skip | ⚠️ | TLA+ no-op; C++ returns private-copy value |
| `read_word`: record version snapshot | L_active: Read (record version) — `read_set ∪ {<<p, xadt_version[p]>>}` | ✅ | |
| `write_word`: CAS acquire ownership | L_active: Write (acquire from free) — `xadt_owner[p] := self` | ✅ | CAS atomics match |
| `write_word`: private copy creation | L_active: Write (update private copy) — `write_set[p] := v` | ⚠️ | Full-page memcpy abstracted to value assign |
| `commit()`: validate read-set versions | L_active: Validate — `xadt_version[p] = ver` | ⚠️ | **Gap**: C++ only checks version; TLA+ also checks `xadt_owner[p] ∈ {0, self}` |
| `commit()`: memcpy write-back | L_writeback: `mem[p] := write_set[p]` | ✅ | |
| `commit()`: fetch_add version | L_release: `xadt_version[p] := xadt_version[p] + 1` | ✅ | |
| `commit()`: store owner=0 | L_release: `xadt_owner[p] := 0` | ✅ | |
| `abort_tx()`: release owner, free private | L_abort: `xadt_owner[p] := 0`, clear sets | ✅ | C++ additionally free()s private pages |
| Bloom filter XF (`xf_set`, `xf_test`) | Not modeled | ❌ | Optimization only |

## Invariants

| Invariant | TLC (seq) | TLC (2t×1p) | TLC (2t×2p) |
|-----------|-----------|-------------|--------------|
 | PageOwnershipExclusion | ✅ PASS | ✅ PASS | ✅ PASS |
 | OwnershipTracked | ✅ PASS | ✅ PASS | ✅ PASS |
 | WriteTrackedOwnership | ✅ PASS | ✅ PASS | ✅ PASS |
 | WritebackConsistent | ✅ PASS | ✅ PASS | ✅ PASS |
 | VersionMonotonic | ✅ PASS | ✅ PASS | ✅ PASS |
 | NoDirtyRead | ✅ PASS | ✅ PASS | ✅ PASS |
 | FenceFidelity | ✅ PASS | ✅ PASS | ✅ PASS |
 | **Combined Inv** | ✅ PASS | ✅ PASS | ✅ PASS |

## Deviations

### 1. Commit validation missing owner_tx_id check (Medium risk)
**C++** (`xtm.hpp:218–229`): Commit-time read-set validation checks only `g_xadt[idx].version.load() != snapshot`. Does **not** check whether another thread currently owns the page.

**TLA+** (lines 117–119): Validation guard requires `xadt_owner[p] = 0 ∨ xadt_owner[p] = self` in addition to version check.

**Risk**: Medium — narrow window exists where read → CAS-acquire by concurrent thread → validate passes on unchanged version → stale commit.

### 2. Bloom filter (XF) not modeled (No risk)
**C++** (`xtm.hpp:71–90`): 8 KB bloom filter for fast negative ownership lookup.

**TLA+**: No bloom filter.

**Risk**: None — performance optimization only.

### 3. Private page allocation abstracted (No risk)
**C++** (`xtm.hpp:160–171, 320–327`): Full 4 KB page `memcpy` on first write and at write-back.

**TLA+**: Single value assignment per page.

**Risk**: None — protocol identical regardless of data granularity.

### 4. Write-set lookup at read: TLA+ no-op vs C++ value return (Low risk)
**C++** (`xtm.hpp:274–278`): Returns private-copy value when page in write-set.

**TLA+**: `skip` — no value tracking.

**Risk**: Low — control flow correct; value irrelevant to protocol invariants.

### 5. Non-TM address bypass (No risk)
**C++** (`xtm.hpp:259–261, 296–299`): `isTMAddress()` guards.

**TLA+**: All addresses in `Page` set.

**Risk**: None — plugin pipeline detail.

### 6. Memory ordering / fence annotations with 3-variable model (Medium risk)
**C++**: Uses `memory_order_acquire`, `memory_order_acq_rel`, `memory_order_release` throughout.

**TLA+**: Three variables (`lastSignalFence`, `lastThreadFence`, `lastRmw`) with `FenceFidelity` invariant covering all three.

**Inaccuracies**:
- Read path: `lastSignalFence = "sc"` where C++ uses `load(acquire)` — not a fence.
- Acquire CAS: `lastRmw = "acquire"` where C++ uses `acq_rel` — loses release half.
- Version bump: `lastRmw = "release"` where C++ uses `store(release)` — not an RMW.
- `lastThreadFence` always `""` — C++ uses `signal_fence` only, never `thread_fence`.

**Risk**: Medium — annotation types are better distinguished than single `lastFence` but still materially wrong on key ordering points.

**TLC verification**: FenceFidelity passes with `(lastSignalFence[t] # "" \/ lastThreadFence[t] # "" \/ lastRmw[t] # "")` — weaker than checking each type individually.

### 7. Nesting / retry accounting (No risk)
**C++** (`xtm.hpp:181, 200–201`): Nested transactions via `tm_nested_call_counter`.

**TLA+**: Single-layer transactions bounded by `MaxCommits`.

**Risk**: None — nesting is runtime convenience.

## Summary

| Aspect | Verdict |
|--------|---------|
| Core commit protocol (validate→write-back→bump→release) | ✅ Good match |
| Ownership CAS acquisition | ✅ Matches; TLA+ guard models CAS atomicity |
| Read-path conflict detection | ✅ Immediate abort on owned page + version snapshot |
| Abort protocol | ✅ Release ownership, clear sets |
| XF bloom filter | ❌ Not modeled (optimization only) |
| Memory ordering / fences | ⚠️ 3-variable model (`lastSignalFence`/`lastThreadFence`/`lastRmw`) but still inaccurate on key points |
| Commit-time owner_tx_id check | ⚠️ C++ missing; TLA+ requires it (Medium) |
| Write-set lookup on read | ⚠️ TLA+ no-op vs C++ private-copy read (Low) |
| Invariant coverage | ✅ 6 invariants all pass TLC across 3 configs |
| **Overall score** | **4/5** |
