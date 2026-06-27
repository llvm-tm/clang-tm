# Audit: SPHT (Scalable Persistent Hardware Transactions)

**Score: 2/5** — TSX retry model fundamentally different (no retry in C++ vs MaxRetries in TLA+); PCL handling in SGL mode diverges (C++ skips PCL entirely); crash/recovery modeled in TLA+ but absent from C++ implementation; DurableValid invariant is buggy (doesn't handle read-only TXs).

## Files

| Artifact | Path | Lines |
|----------|------|-------|
| TLA+ spec | `docs/proofs/SPHT.tla` | 383 |
| C++ header | `backends/tm_impl/spht/SPHT.hpp` | 355 |
| C++ runtime | `backends/tm_impl/spht/SPHT_runtime.cpp` | 381 |
| Implementation notes | `backends/tm_impl/spht/Implementation_notes.md` | 165 |
| TLC config | `docs/proofs/SPHT.cfg` | 19 |

## Algorithm Summary

SPHT uses Intel RTM (TSX) for hardware-accelerated conflict detection with a per-thread commit log (PCL) and epoch-based group commit to amortize NVM flush overhead. When RTM fails (first abort sets `rtm_broken` permanently), the thread falls back to SGL forever. The PCL accumulates writes across multiple HTM transactions; every `GROUP_COMMIT_INTERVAL=16` non-read-only TXs, `group_commit()` issues `clwb`+`sfence` for all PCL entries and publishes the durable TX sequence number.

## Cross-Reference Checklist

| C++ Function/Pattern | TLA+ Action/Label | Match | Notes |
|----------------------|-------------------|-------|-------|
| `spht::init()` (SPHT.hpp:124) | `Init` (SPHT.tla:78) | ⚠️ Partial | C++ inits `g_durable_seqs=nullptr` + `g_num_threads=0`; TLA+ inits all variables (mem, sgl, tsx_mode, pc, pcl, etc.) |
| `spht::init_thread()` (SPHT.hpp:136) | (no equivalent action) | ❌ Missing | TLA+ has no thread-local init; model starts with all threads ready |
| `spht::begin()` — `_xbegin()` returns `_XBEGIN_STARTED` (SPHT.hpp:231) | `TSXBegin(t)` (SPHT.tla:97) | ⚠️ Partial | TLA+ requires `sgl=0`; C++ doesn't check (CPU tracks read-set) |
| `spht::begin()` — TSX aborts, `rtm_broken=true`, returns false (SPHT.hpp:236-241) | `TSXAbort(t)` + `TSXRetryOrFallback(t)` (SPHT.tla:163,176) | ❌ **Major** | C++ sets `rtm_broken=true` permanently — **no TSX retry ever**. TLA+ retries up to `MaxRetries` then falls back. |
| `spht::begin()` — `rtm_broken || !rtm_available()` returns false (SPHT.hpp:217) | `TSXRetryOrFallback` ELSE branch (SPHT.tla:183) | ⚠️ Partial | C++: `rtm_broken` persists across TX reset. TLA+: `MaxRetries` counter resets on SGL fallback. |
| `real_tm_begin()` SGL fallback (SPHT_runtime.cpp:170) | `SGLBegin(t)` (SPHT.tla:199) | ✅ | Both acquire mutual exclusion lock |
| `spht::tm_read()` — direct mem read (SPHT.hpp:279) | `TSXRead(t,a)` / `SGLRead(t,a)` (SPHT.tla:108,209) | ✅ | Direct memory read; no state change |
| `spht::tm_write()` — append to PCL, write to mem (SPHT.hpp:293) | `TSXWrite(t,a,v)` (SPHT.tla:117) | ✅ | Both append to PCL AND write to memory |
| `spht::tm_write()` — SGL/!active path: `*addr=val` only, no PCL (SPHT.hpp:309) | `SGLWrite(t,a,v)` (SPHT.tla:216) | ❌ **Major** | TLA+: `SGLWrite` appends to PCL AND writes mem. C++: SGL write is direct `*addr=val` with **no PCL append** → SGL writes are NOT durable. |
| `spht::commit()` — `_xend()`, tx_seq++, group_commit check (SPHT.hpp:257) | `TSXCommit(t)` (SPHT.tla:129) | ⚠️ Partial | TLA+: GroupCommit triggered at `(tx_seq+1)%GroupInterval=0` unconditionally. C++: `group_commit()` does nothing if `pcl->empty() || read_only`. |
| `group_commit()` flush + epoch table publish + write-back (SPHT.hpp:173) | `GroupCommit(t)` (SPHT.tla:150) | ⚠️ Partial | C++ guards `if (pcl->empty() || read_only) return;`. TLA+ sets `durable_seq` unconditionally. C++: write-back only for LOG_WRITE; alloc/free entries skipped. |
| `spht::abort_tx()` — `_xabort(1)`, `siglongjmp` (SPHT.hpp:244) | `TSXAbort(t)` (SPHT.tla:163) | ⚠️ Partial | TLA+: clears `tsx_buffer`, inc retry_cnt, sets pc=aborting. C++: just `_xabort(1)` + longjmp (hardware rolls back). |
| PCL truncation on abort (no code) | `TSXRetryOrFallback` — pops stale entries (SPHT.tla:189) | ❌ **Missing** | TLA+ explicitly removes entries between `pcl_epoch_start` and `Len(pcl)`. C++ relies on TSX hardware to roll back PCL writes (both mem and PCL are inside TSX region). Correct only for TSX path; SGL aborts leave stale PCL entries. |
| (no crash/recovery in C++) | `Crash` + `Recovery(t)` (SPHT.tla:248,258) | ❌ **Missing** | C++ has no crash/recovery implementation. TLA+ models system crash with full PCL replay. |
| `real_tm_begin()` — `tm_longjmp_ret != 0` guard (SPHT_runtime.cpp:178) | (no equivalent) | ❌ Missing | Runtime handles siglongjmp retry edge case (skip SGL if longjmp just happened) |
| `g_durable_seqs` atomic array (SPHT.hpp:111) | `durable_seq[t]` variable (SPHT.tla:62) | ⚠️ Partial | C++: ring buffer of 64 entries indexed by `tx_seq%64`. TLA+: per-thread scalar variable. |
| `rtm_broken` flag (SPHT.hpp:96) | (no equivalent) | ❌ Missing | Not modeled. Affects whether TSX is attempted at all. |

## Invariants

TLC configuration: `Thread={1,2}`, `Addr={0,1}`, `Data={0,1}`, `MaxRetries=2`, `GroupInterval=2`.

| Invariant | TLA+ expression | TLC result | Notes |
|-----------|-----------------|------------|-------|
| `TSXSafety` | `tsx_mode[t] ⇒ sgl=0` | ✅ PASS | |
| `LockExclusion` | `sgl=t1 ∧ sgl=t2 ⇒ t1=t2` | ✅ PASS | |
| `LockOwnerInv` | `sgl=t ⇒ pc[t]∈{"active_sgl","commit_sgl"}` | ✅ PASS | |
| `TSXvsSGLSafety` | `sgl≠0 ⇒ ¬tsx_mode[t]` | ✅ PASS | |
| `DurableSeqMonotonic` | `durable_seq[t] ≤ tx_seq[t]` | ✅ PASS | |
| `PCLBounds` | `1 ≤ pcl_epoch_start[t] ≤ Len(pcl[t])+1` | ✅ PASS | |
| `DurableValid` | `durable_seq[t] ≤ Len(pcl[t])` | ❌ **FAIL** | Violated when read-only TX triggers GroupCommit: `durable_seq=2` but `Len(pcl)=0`. Model bug: `durable_seq` is a TX counter, not a PCL index. |
| `AtMostOneMode` | `¬(tsx_mode[t1] ∧ pc[t2]="active_sgl" ∧ sgl≠0)` | ✅ PASS | |
| `TSXBufferInUse` | `HasWrittenInTSX(t,a) ⇒ tsx_mode[t]` | ✅ PASS | |

**Result**: 8/9 invariants pass. `DurableValid` fails because it conflates `durable_seq` (TX sequence number) with `Len(pcl)` (PCL entry count). After GroupCommit, `durable_seq` equals `tx_seq` but `pcl` may be empty (read-only TXs). C++ `group_commit()` avoids this with an explicit `if (pcl->empty() || read_only) return;` guard.

## Deviations

### 1. TSX retry model: no retry vs MaxRetries (High risk)

**TLA+** (`SPHT.tla:176-192`): After TSX abort, `TSXRetryOrFallback` retries TSX up to `MaxRetries` times (default 2 in model, configurable), then falls back to SGL. PCL entries from the aborted TX are explicitly popped.

**C++** (`SPHT.hpp:217-241`): On first TSX abort, `begin()` sets `tx->rtm_broken = true` immediately (even on first failure). All subsequent calls to `begin()` skip RTM entirely and return `false`. No retry. The fallback in `real_tm_begin()` (SPHT_runtime.cpp:178-183) acquires the SGL mutex unconditionally when `begin()` returns false (with a guard for `tm_longjmp_ret != 0`).

**Risk: High** — The C++ design is strictly more conservative (abort once → SGL forever). The TLA+ model allows multiple TSX attempts, exploring states the C++ can never reach. This means model-checked safety properties may not hold for the C++ implementation in the reverse direction (the model is a superset of the C++ behavior for retry policy, but the PCL abort handling differs).

### 2. SGL writes bypass PCL entirely (High risk)

**TLA+** (`SPHT.tla:216-225`): `SGLWrite` appends to PCL AND writes to memory: `pcl' = Append(pcl[t], <<a, v>>)` and `mem' = [mem EXCEPT ![a] = v]`.

**C++** (`SPHT.hpp:309-311`): In SGL/!active mode, `tm_write()` executes `*addr = val` with **no PCL append**. The function returns early: `if (!current_tx || !current_tx->active) { *addr = val; return; }`. PCL is only appended when `current_tx && current_tx->active` (i.e., inside TSX).

**Risk: High** — The TLA+ assumes SGL writes are durable (via PCL), but C++ SGL writes have zero durability tracking. If the system crashes during SGL mode, those writes are lost from the durable log. The model would validate recovery invariants that C++ cannot satisfy.

### 3. No crash/recovery in C++ (Medium risk)

**TLA+** (`SPHT.tla:248-284`): Full `Crash` and `Recovery(t)` actions. `Crash` sets all threads to crashed state (excluding mid-group_commit). `Recovery(t)` replays durable PCL entries to memory.

**C++**: No crash/recovery implementation. The `g_durable_seqs` array is allocated in `init_thread()` but never used for recovery. The `Implementation_notes.md` describes recovery in theory (parallel log replay, durable epoch table), but no code exists.

**Risk: Medium** — The TLA+ spec's durability invariants (`DurableRedo`, `RecoveryCorrect` from comments, `RecoveryConsistency` temporal property) cannot be validated against C++ because the C++ doesn't implement recovery. The model is a forward-looking design spec, not an implementation verification.

### 4. DurableValid invariant is buggy (Medium risk)

**TLA+** (`SPHT.tla:344-346`): `DurableValid == \A t \in Thread : durable_seq[t] <= Len(pcl[t])`.

**Problem**: `durable_seq` stores a TX sequence number (monotonically increasing per transaction), but `Len(pcl)` stores the number of PCL entries. After a read-only TX followed by GroupCommit, `durable_seq = 2` but `Len(pcl) = 0`, violating the invariant.

**C++** (`SPHT.hpp:176`): `group_commit()` has explicit guard `if (pcl->entries.empty() || tx->read_only) return;` — so the C++ never enters this state. The model should either add this guard or reformulate the invariant.

**Risk: Medium** — Model bug means the invariant as-written is not useful for C++ verification. The C++ guard prevents the violation, but the model doesn't capture it.

### 5. PCL truncation on TSX abort: hardware rollback vs explicit removal (Low risk)

**TLA+** (`SPHT.tla:189-190`): `TSXRetryOrFallback` explicitly pops stale PCL entries: `pcl' = [pcl EXCEPT ![t] = [i \in 1..KeepLen |-> pcl[t][i]]]`.

**C++**: No explicit PCL truncation on abort (`abort_tx()` at SPHT.hpp:244 just calls `_xabort(1)` + `siglongjmp`). The PCL `append()` was inside the TSX transaction, so RTM hardware rollback restores the PCL vector to its pre-TSX state.

**Risk: Low** — The behavioral effect is the same (stale entries discarded). The mechanism differs (hardware vs explicit). Correct as long as TSX is used. However, for SGL aborts (which don't have hardware rollback), C++ would leave stale PCL entries — but C++ doesn't write to PCL in SGL mode (Deviation 2), so this is moot.

### 6. group_commit has empty/read-only guard not in model (Low risk)

**TLA+** (`SPHT.tla:150-161`): `GroupCommit` always sets `durable_seq[t] = tx_seq[t]` and updates `pcl_epoch_start`.

**C++** (`SPHT.hpp:176`): `if (pcl->entries.empty() || tx->read_only) return;` — first line of `group_commit()`.

**Risk: Low** — The C++ guard prevents NVM work for empty PCLs. In the TLA+, `GroupCommit` would update `durable_seq` even for empty PCLs (causing the `DurableValid` violation). Minor guard; affects performance more than correctness.

### 7. rtm_broken persistent flag not modeled (Low risk)

**C++** (`SPHT.hpp:96`): `rtm_broken` is a persistent per-thread flag that survives across `reset()` calls. Once set (by the first TSX abort), the thread never attempts TSX again.

**TLA+**: No equivalent flag. The model's `TSXRetryOrFallback` resets `retry_cnt` on SGL begin, allowing future TSX attempts.

**Risk: Low** — The model over-approximates TSX attempts (more aggressive than C++). This doesn't introduce unsafe states — C++ is strictly more conservative.

### 8. Per-thread durable_seq: atomic ring buffer vs scalar variable (No risk)

**C++** (`SPHT.hpp:111`, SPHT.hpp:186): `g_durable_seqs` is an array of 64 `atomic<uint64_t>` entries, indexed by `tx_seq % 64`. On group commit: `g_durable_seqs[idx].store(tx->tx_seq, ...)`.

**TLA+** (`SPHT.tla:62`): `durable_seq` is a simple `[Thread -> Nat]` variable.

**Risk: None** — The ring buffer is an implementation detail for bounded space. The relevant property (published sequence number) is preserved.

### 9. Null-address guard in C++, not modeled (No risk)

**C++** (`SPHT.hpp:297-298`): `if (!addr || (uintptr_t)addr < 0x100000 || ((uintptr_t)addr >> 47) != 0) return;`.

**TLA+**: All `Addr` values are valid (`Addr \subseteq Nat`). No invalid-address handling.

**Risk: None** — Standard abstraction. The TLA+ address space only contains valid TM-tracked addresses.

### 10. Pass-through mode (non-RTM CPU) not modeled (No risk)

**C++** (`SPHT.hpp:217-218`): `if (tx->rtm_broken || !rtm_available()) { tx->active = false; return false; }` — on CPUs without RTM, `begin()` immediately returns false and SGL is used.

**TLA+**: No non-RTM scenario. All executions have TSX available.

**Risk: None** — SGL fallback path handles this. Model over-approximates TSX availability but the SGL path exists in both.

## Summary

| Aspect | Verdict |
|--------|---------|
| Dual-mode protocol (TSX/SGL) | ⚠️ Partial — SGL PCL handling differs |
| TSX retry model | ❌ Major — no retry vs MaxRetries |
| Crash/recovery | ❌ Missing from C++ |
| DurableValid invariant | ❌ Buggy (read-only TX violation) |
| Invariants (non-DurableValid) | ✅ 8/9 pass TLC |
| PCL abort cleanup | ⚠️ Hardware vs explicit (equivalent) |
| Group commit epoch logic | ⚠️ Missing read_only/empty guard in model |
| Pass-through mode | ❌ Not modeled (non-RTM CPUs) |
| rtm_broken flag | ❌ Not modeled |
| Null-address/LlvmTmPlugin guards | ❌ Not modeled (standard abstraction) |
| **Overall score** | **2/5** |

The SPHT TLA+ spec is aspirational — it models the paper's design (including crash/recovery with PCL replay) rather than the C++ implementation. The two fundamental gaps are: (1) the C++ disables TSX permanently after the first abort (`rtm_broken=true`) while the model retries up to `MaxRetries`, and (2) the C++ SGL path bypasses the PCL entirely (no durability tracking), while the model logs SGL writes to the PCL. An accurate model would need to either match the C++ "one abort = permanent SGL" pattern or add the `rtm_broken` flag to the state. The `DurableValid` invariant should be removed or reformulated.
