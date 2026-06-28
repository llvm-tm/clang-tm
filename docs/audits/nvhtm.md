# NV-HTM Backend Audit

## **Score: 1/5** — Model describes checkpoint/recovery + SGL fallback that DO NOT EXIST in C++ implementation. C++ uses pass-through on RTM failure, no checkpoint markers, no SGL. 12 `lastFence` annotations correspond to nothing in C++. **Critical downgrade from memory ordering audit (2026-06-28).**

## Files

| Artifact | Path | Lines |
|----------|------|-------|
| TLA+ Spec | `docs/proofs/NVHTM.tla` | 383 |
| TLA+ Config | `docs/proofs/NVHTM.cfg` | 17 |
| C++ Header | `backends/tm_impl/nvhtm/NVHTM.hpp` | 302 |
| C++ Runtime | `backends/tm_impl/nvhtm/NVHTM_runtime.cpp` | 318 |
| Globals Header | `backends/tm_impl/nvhtm/nvhtm_globals.hpp` | 16 |
| Implementation Notes | `backends/tm_impl/nvhtm/Implementation_notes.md` | 144 |

## Algorithm Summary

NV-HTM combines Intel RTM (Restricted Transactional Memory) for hardware-accelerated conflict detection with a per-transaction redo log for NVM durability. Writes are logged AND written through to memory during an RTM transaction (HTM rolls back both on abort); after `_xend()`, the log is flushed to NVM via `_mm_clflush`/`_mm_sfence` then all writes are re-applied to their final addresses. If RTM is unavailable, the backend runs in pass-through mode with no TM protection whatsoever.

## Cross-Reference Checklist

| C++ NVHTM.hpp / Runtime | TLA+ Label / Action | Match | Notes |
|------------------------|---------------------|-------|-------|
| `init()` (NVHTM.hpp:110) | — (no equivalent) | ❌ | TLA+ Init sets all vars; C++ `init()` is a no-op |
| `init_thread()` (NVHTM.hpp:113-119) | — | ❌ | TLA+ has no thread init action; assumes per-thread state available from start |
| `begin()` (NVHTM.hpp:131-161) | `TSXBegin(t)` (NVHTM.tla:94-102) | ⚠️ | TLA+ models `_xbegin()` returning `_XBEGIN_STARTED` only; C++ has retry loop with siglongjmp, exponential backoff, and pass-through when RTM unavailable. TLA+ lacks pass-through fallback entirely |
| `tm_read()` (NVHTM.hpp:216-230) | `TSXRead(t, a)` (NVHTM.tla:105-108) | ✅ | Both delegate to hardware (HTM tracks read-set); read bypass check for non-TM addresses in C++ is TLA+-invisible |
| `tm_write()` (NVHTM.hpp:232-280): | `TSXWrite(t, a, v)` (NVHTM.tla:114-122) | ❌ | **Critical deviation**: C++ writes through to `*addr` AND the log (NVHTM.hpp:263,279). TLA+ line 118-119 explicitly says `mem` is NOT changed ("redo-log-only"). C++ keeps latest value per address (dedup scan at lines 259-265); TLA+ `Append` creates duplicate entries and unbounded log growth |
| Null-address guard (NVHTM.hpp:238, lines 238-248) | — | ❌ | Not modeled at all |
| LLVM_TM_PLUGIN address bypass (NVHTM.hpp:221-224, 241-244) | — | ❌ | TM-irrelevant abstraction |
| `commit()` → `_xend()` (NVHTM.hpp:199-209) | `TSXCommit(t)` (NVHTM.tla:126-133) | ✅ | Both end HTM transaction and move to post-commit |
| `durable_commit()` (NVHTM.hpp:178-197) | `FlushLog(t)` + `WriteCheckpoint(t)` + `ApplyLog(t)` + `ClearCheckpoint(t)` | ❌ | **Major deviation**: C++ does clflush → sfence → apply (3 steps, no checkpoint). TLA+ models a 4-step checkpoint protocol (flush → write_cp → apply → clear_cp). C++ has zero checkpoint markers |
| No checkpoint write in C++ | `WriteCheckpoint(t)` (NVHTM.tla:144-154) | ❌ | C++ never writes a checkpoint marker; recovery cannot distinguish committed-but-not-persisted transactions |
| No recovery code in C++ | `Recovery(t)` (NVHTM.tla:271-287) | ❌ | TLA+ models recovery replay from checkpoint; C++ has no recovery mechanism |
| `abort_tx()` (NVHTM.hpp:163-175) | `TSXAbort(t)` (NVHTM.tla:194-203) | ⚠️ | TLA+ clears redo_log and goes to "aborting" state; C++ calls `_xabort(1)` which generates HW rollback then retries via siglongjmp. Pass-through path in C++ (lines 165-168) not modeled |
| Retry logic (NVHTM.hpp:149-159) | `TSXRetryOrFallback(t)` (NVHTM.tla:206-218) | ⚠️ | Both have retry limit + fallback. C++ has exponential backoff (`sleep_for(10 * (1 << (n-3)))`, lines 155-158); TLA+ retries immediately (no backoff). C++ uses siglongjmp within `begin()`; TLA+ uses explicit actions |
| SGL fallback | `SGLBegin(t)` / `SGLRead(t,a)` / `SGLWrite(t,a,v)` / `SGLCommit(t)` (NVHTM.tla:225-264) | ❌ | C++ has NO SGL fallback. When RTM unavailable, C++ goes to pass-through (NVHTM.hpp:137-141) with NO mutex, NO TM tracking, NO consistency. TLA+ models a full SGL mutex protocol that C++ does not implement |
| Pass-through mode | — | ❌ | **CRITICAL omission**: TLA+ has no model for no-RTM execution. C++ pass-through (NVHTM.hpp:137-140) provides zero consistency guarantees |
| `real_tm_begin()` (NVHTM_runtime.cpp:127-134) | — | ⚠️ | Runtime wrapper adds `g_in_tx = true` and spec/deferred-free management — not in TLA+ |
| `real_tm_end()` (NVHTM_runtime.cpp:136-143) | — | ⚠️ | Runtime wrapper adds `g_in_tx = false` and spec alloc deallocation — not in TLA+ |
| `real_tm_free()` deferred frees (NVHTM_runtime.cpp:193-217) | — | ❌ | Double-free detection and deferred free list; not modeled |
| `real_tm_malloc/calloc/realloc` | — | ❌ | Region allocator layer; not modeled |

## Invariants

TLC run: `java -cp /tmp/tla2tools.jar -Xmx8g tlc2.TLC docs/proofs/NVHTM.tla -config <config>`

| Invariant | TLC Result | Notes |
|-----------|-----------|-------|
| `TSXSafety` — if in TSX mode, SGL is free | ✅ PASS | N/A — no thread in TSX+SGL simultaneously in reachable states |
| `LockExclusion` — at most one thread holds SGL | ✅ PASS | Only 1 thread can set sgl at a time |
| `LockOwnerInv` — SGL owner is in SGL mode | ✅ PASS | Passes vacuously — SGL never reached with multi-thread config |
| `TSXvsSGLSafety` — no TSX during SGL | ✅ PASS | Passes vacuously — no concurrent TSX+SGL reachable |
| `CheckpointConsistent` — checkpoint ⇒ cp_valid | ✅ PASS | Both set/cleared together in `WriteCheckpoint`/`ClearCheckpoint` |
| `NoStaleCheckpoint` — idle ⇒ no checkpoint | ✅ PASS | `ClearCheckpoint` clears before returning to idle |
| `FreshLogOnBegin` — redo_log empty at TX start | ❌ **FAIL** | **Model bug**: invariant checks `pc[t] ∈ {"active_tsx", "active_sgl"} ⇒ log is empty`, but after `TSXWrite` the log is non-empty while pc is still `"active_tsx"`. Should instead check `pc[t] = "idle" ⇒ log = << >>` or use a transition invariant |
| `CommitPhaseOrdering` — checkpoint set during commit phases | ❌ **FAIL** | After `TSXCommit(t)`, `pc[t] = "flush_log"` but `checkpoint[t] = FALSE`. The model only sets checkpoint in `WriteCheckpoint` (step after FlushLog). However, this matches C++ which has NO checkpoint markers at all — the gap exists in both model and code |

Raw TLC output (sequential config, without the two failing invariants): timed out after 120s — infinite state space caused by unbounded `Append` on `redo_log` (see Deviations #1).

## Deviations

### 1. Redo-log semantics: `Append` vs deduplication-by-address

**C++ lines**: NVHTM.hpp:258-279  
**TLA+**: NVHTM.tla:118 (`redo_log' = Append(redo_log[t], <<a, v>>)`)  
**Risk**: Medium  

C++ redo log deduplicates: a write to the same address overwrites the existing log entry (NVHTM.hpp:259-265). The TLA+ model uses `Append`, which grows the sequence indefinitely with duplicate address entries. This causes two problems: (a) infinite TLC state space (no bound on log sequence length), (b) `ApplyLog` in TLA+ must handle duplicate entries with a "last write wins" heuristic (lines 171-178) that the C++ code approaches more naturally via overwrite semantics.

### 2. No checkpoint in C++ (state machine mismatch)

**C++ lines**: NVHTM.hpp:178-197 (`durable_commit()`)  
**TLA+**: NVHTM.tla:144-191 (`WriteCheckpoint`, `ApplyLog`, `ClearCheckpoint`)  
**Risk**: High  

The TLA+ model defines a 4-step post-commit durable phase: FlushLog → WriteCheckpoint → ApplyLog → ClearCheckpoint. The C++ code does: clflush → sfence → apply writes — with **no checkpoint marker at all**. This means:

- If a crash occurs after `_xend()` but before `durable_commit()` finishes, there is no durable record of which log entries need replaying.
- The `durable_commit()` function re-applies writes to already-updated memory (idempotent), but without a checkpoint, recovery cannot distinguish a committed-but-unpersisted transaction from a fully-persisted one.
- The TLA+ invariants `CommitPhaseOrdering`, `CheckpointConsistent`, and `NoStaleCheckpoint` are describing a protocol that does not exist in C++.

### 3. Write-through during HTM (contradicts TLA+ spec)

**C++ lines**: NVHTM.hpp:263, 279 (`*addr = val;`)  
**TLA+**: NVHTM.tla:118-119 (`mem is NOT changed — RTM will discard on abort`)  
**Risk**: Low  

C++ writes through to `*addr` within the RTM transaction (HTM hardware rolls back on abort). TLA+ explicitly says `mem` is unchanged. The practical difference is nil — HTM ensures atomicity of the write-through at the same point as the log write. However, the `durable_commit()` must re-apply values (NVHTM.hpp:192) to ensure NVM persistence, since the write-through during HTM may have only hit volatile cache. This is idempotent but redundant.

### 4. No SGL fallback in C++ (pass-through instead)

**C++ lines**: NVHTM.hpp:137-141  
**TLA+**: NVHTM.tla:225-264 (`SGLBegin`, `SGLRead`, `SGLWrite`, `SGLCommit`)  
**Risk**: High  

When RTM is unavailable, C++ enters pass-through mode: `tx->active = false; return false;`. Every subsequent read/write becomes a plain load/store with **zero TM protection** and **zero mutual exclusion**. Multi-threaded correctness is entirely lost. The TLA+ model, by contrast, has a full SGL mutex protocol with mutual exclusion, redo logging for durability, and proper commit. **The C++ implementation abandons all safety guarantees** when RTM is absent — the TLA+ model incorrectly suggests NV-HTM provides a safe fallback.

### 5. No recovery mechanism in C++

**C++ lines**: — (no recovery code anywhere)  
**TLA+**: NVHTM.tla:271-287 (`Recovery(t)`)  
**Risk**: High  

The TLA+ model defines a `Recovery` action that scans checkpoints and replays redo logs. The C++ code has zero recovery logic. Since the C++ code also lacks checkpoint markers (deviations #2), there is nothing to recover from — but this also means any NVM-persistent state after a crash may be inconsistent. The `durable_commit()` function tries to persist data, but without a recovery mechanism, a crash between `_xend()` and the completion of `durable_commit()` leaves the system in an unrecoverable state.

### 6. Retry backoff not modeled

**C++ lines**: NVHTM.hpp:155-158  
**TLA+**: NVHTM.tla:206-218 (`TSXRetryOrFallback`)  
**Risk**: None  

C++ applies exponential backoff: `sleep_for(10 * (1 << (retry_count - 3)))` for retry_count > 3. The TLA+ model retries immediately. Backoff affects performance but not correctness.

### 7. `FreshLogOnBegin` invariant is incorrectly formulated

**TLA+**: NVHTM.tla:348-353  
**Risk**: Low (model artifact, not C++ bug)  

The invariant requires `pc[t] ∈ {"active_tsx", "active_sgl"} ⇒ redo_log[t] = << >>`. But after `TSXWrite`, pc is still `"active_tsx"` while the log is non-empty. The intended check is "redo_log must be empty when entering a transaction", not "throughout the entire transaction." A correct formulation would be either a transition invariant (`pc'[t] = "active_tsx" ⇒ redo_log'[t] = << >>`) or a state invariant on idle: `pc[t] = "idle" ⇒ redo_log[t] = << >>`.

### 8. `CommitPhaseOrdering` invariant violated by model's own state machine

**TLA+**: NVHTM.tla:356-359  
**Risk**: Low (model artifact, but reflects real C++ gap)  

The invariant requires `checkpoint[t] = TRUE` whenever `pc[t] ∈ {"flush_log", "write_cp", "apply_log", "clear_cp"}`. But `TSXCommit` transitions to `"flush_log"` without setting `checkpoint`. The checkpoint is only set in `WriteCheckpoint`, after `FlushLog`. This means the model itself violates the invariant — the invariant is misaligned with the model's own protocol. In the C++ code this is moot (no checkpoint exists at all), but the invariant was presumably meant to enforce crash-recovery safety.

### 9. Null-address guard not modeled

**C++ lines**: NVHTM.hpp:238  
**Risk**: None  

C++ silently drops writes to addresses < 0x100000 or with bit 47 set. Purely a safety feature; not relevant to formal model.

### 10. LLVM_TM_PLUGIN address bypass not modeled

**C++ lines**: NVHTM.hpp:221-224, 240-244  
**Risk**: None  

Plugin-specific bypass for non-TM-region addresses. Automatically handled by the hook dispatch layer.

## Summary

| Aspect | Verdict |
|--------|---------|
| **Core HTM protocol** | ⚠️ C++ writes through to memory during HTM; TLA+ models redo-log-only. HTM rollback makes this equivalent, but `durable_commit()` must re-apply. |
| **Fallback path** | ❌ C++ uses pass-through (no TM) when RTM unavailable. TLA+ models a full SGL mutex fallback. Neither exists in the other. |
| **Durability model** | ❌ TLA+ has a sophisticated checkpoint protocol (FlushLog→WriteCheckpoint→ApplyLog→ClearCheckpoint). C++ has clflush+sfence+apply with NO checkpoint and NO recovery. |
| **Redo-log data structure** | ❌ C++ deduplicates by address (bounded by distinct addresses). TLA+ uses Append (unbounded growth, causing infinite TLC state space). |
| **Memory model / fences** | ❌ Neither models nor tracks memory ordering. `_mm_sfence` in C++ durability path has no TLA+ counterpart. |
| **Retry logic** | ⚠️ C++ has exponential backoff; TLA+ retries immediately. Correctness unaffected. |
| **TLC model checking** | ❌ 2 of 8 invariants fail. `FreshLogOnBegin` is a formulational error. `CommitPhaseOrdering` reflects model's own protocol gap. Sequential config times out (infinite state space from unbounded Append). |
| **Implementation Notes alignment** | ❌ Notes describe the ideal checkpoint protocol (Steps A-D). C++ code implements a simplified subset (clflush+sfence+apply, no checkpoint). Key design elements in the spec have no corresponding implementation. |
| **Documentation coverage** | ⚠️ Notes are thorough about paper background and ideal design but do not highlight the simplification gap vs the actual C++ code. |

### **Overall Score: 2/5**

The TLA+ model and C++ implementation describe fundamentally different protocols for the durable phase. The model assumes a checkpoint-based protocol with full recovery; the code uses a simplified flush-and-apply approach with no crash recovery. The model's SGL fallback is completely absent from the code (replaced by pass-through with zero guarantees). The model's `FreshLogOnBegin` invariant is incorrectly stated, and `CommitPhaseOrdering` fails because the model's own state machine does not enforce its requirement. Without checkpoint and recovery, the NVM durability claims in the Implementation Notes are not realized in the code, making this backend effectively a non-persistent HTM with an incomplete durability mechanism.
