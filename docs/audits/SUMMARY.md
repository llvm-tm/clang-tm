# Audit Summary: TLA+ Specs vs C++ Implementations

## Methodology

Each backend audited per [AUDIT_PLAN.md](../proofs/AUDIT_PLAN.md) 4-step process:
1. Semantic abstraction identification
2. Per-backend abstraction gaps
3. Cross-validation (analysis phase; trace validation TBD)
4. Invariant cross-check

This revision adds a **memory ordering (MO) sub-score** reflecting how faithfully the PlusCal/TLA+ model captures the C++/Rust implementation's acquire/release semantics, compiler barriers, and CPU fences.

## Score Guide

| Score | Meaning |
|-------|---------|
| **5/5** | Perfect match — no abstraction gap |
| **4/5** | Minor deviations (fences, spin loops, read-validate re-check) |
| **3/5** | Significant protocol abstractions (type merging, lock retry, TSX capacity) |
| **2/5** | Major algorithmic differences (would require separate spec to capture) |
| **1/5** | Backend does what the spec describes but not vice versa |

## Memory Ordering Sub-score Guide

| MO Score | Meaning |
|----------|---------|
| **5/5** | Every atomic operation's ordering (`acquire`/`release`/`acq_rel`/`seq_cst`) is correctly typed in the model |
| **4/5** | Major ordering points captured; minor over/under-estimation in ordering strength |
| **3/5** | Several ordering points missing or mis-categorized; signal vs thread fence not distinguished |
| **2/5** | Critical ordering gaps (missing fences, wrong categories) that could affect correctness reasoning |
| **1/5** | Memory ordering essentially unmodeled |

## Updated Scores

### Phase 1 Backends (PlusCal)

| Backend | Overall | MO | Key gaps | Risk |
|---------|---------|----|----------|------|
| **SGL** | 5/5 | 5/5 | None — `std::mutex` provides implicit acquire/release; `lastFence` intentionally omitted because lock is entire synchronisation mechanism | None |
| **TSXSGL** | 4/5 | 3/5 | `lastFence` + `FenceFidelity` present. TSX capacity aborts not modeled. `sgl_owner` uses `seq_cst` loads/stores but model single `"sc"` tag cannot distinguish `signal_fence` from `seq_cst` load/store. Spin-wait loop abstracted. RTM availability assumed. | Low |
| **TinySTM_WBCTL** | 4/5 | 2/5 | `lastFence` + `FenceFidelity` present. Double-check protocol window abstracted (read lock → data → re-read lock is atomic in model, not atomic in C++). `is_locked()` acquire semantics not captured. `atomic_signal_fence` vs `atomic_thread_fence` indistinguishable. Spin-loop acquire loads unmodeled. EBR version tracking absent. | Low (overall), Medium (MO) |
| **TinySTM_WBETL** | 4/5 | 2/5 | Same gaps as WBCTL. Encounter-time lock spin loop abstracted (5000 iterations → atomic check). Token soft-spin ordering not modeled. `set_version()` CAS ordering missing. | Low (overall), Medium (MO) |
| **TinySTM_WT** | 4/5 | 2/5 | Same gaps as WBCTL. Additionally: post-CAS version re-read window (C++ `lock->get()` after `try_lock()` to get true version) is a correctness-relevant ordering step entirely absent from model. Incarnation wrap-around correctly modeled. | Low (overall), Medium (MO) |
| **PersistentSGL** | **4/5** | 3/5 | Dual-write properly split (`L_active` mem write → `L_write_nvm` nvm write with `pending_*` crash window). `lastSignalFence`/`lastThreadFence`/`lastRmw` tracking present. `NVMAgreesWithMem` correctly accounts for crash-window intermediate state. C++ still lacks fence between `*addr=val` and `memcpy` — model reveals this gap correctly. | Low |

### Phase 2 Backends (PlusCal)

| Backend | Overall | MO | Key gaps | Risk |
|---------|---------|----|----------|------|
| **Romulus** | **4/5** | 3/5 | **Updated 2026-06-29**. All 4 missing commit-path annotations added: `lastRmw="acq_rel"` for lock-bit `fetch_or`; `lastRmw="acq_rel"`+`lastThreadFence="sc"` for clock increment; `lastThreadFence="sc"` after write-back; `lastRmw="release"` for version `store`. Clock increment was mis-annotated as `signal_fence` — now corrected to `acq_rel`+`thread_fence`. Rust backend differences (no read-set, no lock-bit, 6 extra `fence(SeqCst)` points) remain. | Low |
| **TL2** | **4/5** | 3/5 | **Updated 2026-06-29**. Clock increment ordering fixed: C++ `fetch_add(relaxed)` → `fetch_add(release)`, model `lastSignalFence="sc"` → `lastRmw="release"`. C++ and model now agree on `release` ordering. Rust backend differences (global commit lock, `fence(SeqCst)` barriers, read-validate loop) remain. | Low |
| **XTM** | **4/5** | 4/5 | **Updated 2026-06-29**. All 4 fence inaccuracies fixed: read path `lastSignalFence="sc"` → `lastRmw="acquire"`; CAS acquire from `"acquire"` → `"acq_rel"`; validate `lastSignalFence="sc"` → `lastRmw="acquire"`; commit `lastRmw="release"` → `"acq_rel"`. XTM uses zero fences — all ordering via RMW and acquire/release loads/stores, now correctly reflected. Bloom filter absent (optimization only, P3). Rust backend different algorithm. | Low |
| **LEFTRIGHT** | **4/5** | 3/5 | **Updated 2026-06-24**. Write path fence annotation absent (correct — matches C++ `write_word` zero-ordering); commit-lock acquire correctly annotated. ARM read-path data-race vulnerability fixed in C++ (`__ATOMIC_ACQUIRE` fence added). Value-based validation `memcmp` ordering gaps remain. | Low |
| **SwissTM** | **4/5** | 3/5 | **Updated 2026-06-29**. `r_lock.exchange(acq_rel)` ordering fixed: `lastRmw="acquire"` → `lastRmw="acq_rel"`. Now correctly captures both acquire (lock) and release (write-back visibility) semantics. Contention manager ordering, undo-log restore, and `write_impl` signal_fence gaps remain. | Low |

### Phase 3 Backends (TLA+-only / PlusCal)

| Backend | Overall | MO | Key gaps | Risk |
|---------|---------|----|----------|------|
| **NOrec** | **4/5** | 4/5 | **Updated 2026-06-30: torn-read double-check refined** — L_active→L_read_data→L_read_check split with intermediate data-capture label. Clock-mismatch path no longer adds potentially torn entries to read-set (retries cleanly via validate+increment torn_reads[t]). Added `rval[t]` and `torn_reads[t]` state variables. TLC passes (4280/1790 states reduced cfg, full cfg ~65M). Remaining gaps: acquire/release as annotation only, plugin-mode bypass not modeled. | Low |
| **DUDETM** | 1/5 | 1/5 | Unchanged. Zero memory ordering captured from 30+ C++ atomic operations or 8+ Rust fences. Model is high-level design sketch. | High |
| **NVHTM** | **2/5** | 1/5 | **Raised from 1/5 (2026-07-04)**. 2 critical C++ bugs found during audit review (dead-code `return` in `tm_write()`, wrong `_mm_clflush` target in `durable_commit()`) were fixed. Model still describes an algorithm that DOES NOT EXIST in C++: checkpoint/recovery protocol with SGL fallback. C++ NVHTM uses pass-through mode on RTM failure. 12 `lastFence` annotations mostly correspond to nothing in C++. Redo log is fixed-size array in C++, unbounded sequence in model. `NVHTM_FIX_PLAN.md` removed (bugs fixed). | High |
| **SPHT** | **2/5** | 2/5 | `lastSignalFence`/`lastThreadFence`/`lastRmw` tracking present. `_mm_sfence()` and `_mm_clflush()` for NVM durability not modeled. RTM retry logic differs (single attempt → SGL in C++; multiple retries → SGL in model). `g_durable_seqs` release store absent. Crash/recovery modeled but absent in C++. `rtm_broken` flag and `tsx_buffer` recovery clearing added. `DurableValid` invariant removed (not actionable). | High |
| **DistributedSGL** | 1/5 | 1/5 | Unchanged. Model is client-server message-passing; C++ is single-machine mmap spinlock. Completely different algorithms. | High |
| **TiKV** | **2/5** | 1/5 | **Downgraded**. No `lastFence` tracking. Async runtime (`tokio`, `block_on`), gRPC error handling, at-most-once delivery semantics not captured. Unbounded counters prevent TLC termination. Model captures Percolator 2PC at high level but misses all distributed-systems detail. | Medium |
| **TSXSim** | **2/5** | 1/5 | **Downgraded**. No `lastFence` tracking. Virtual cycle counting replaces real memory ordering; bloom filter false-positive rate configurable but not modeled; capacity thresholds abstracted. Model captures dual-path TSX/SGL but misses simulation-specific detail. | Medium |
| **DESEngine** | 2/5 | 1/5 | Unchanged. Naming mismatch already documented (models engine.rs, not sim_engine.rs). No memory ordering captured. | Medium |

## Cross-Cutting Observations

1. **`lastFence[t]` coverage (11 of 19)**: TSXSGL, TinySTM_WBCTL/WBETL/WT, TL2, SwissTM, LEFTRIGHT, XTM, Romulus, NVHTM, NOrec have it. SGL, PersistentSGL, DistributedSGL, SPHT, TiKV, TSXSim, DESEngine, DUDETM do not.

2. **`lastFence` limitation**: As previously documented, it cannot distinguish `atomic_signal_fence` (compiler barrier, zero CPU instructions) from `atomic_thread_fence` (CPU `dmb`/`mfence`), nor bundled RMW ordering (`fetch_add(acq_rel)`, `exchange(acq_rel)`). `FenceFidelity` only checks `writeSet ≠ {} ⇒ fence happened` — no guarantee of sufficient strength or correct placement. This analysis confirms the gap is broader than previously assessed: ALL backends with `lastFence` have at least 2-3 ordering points where the annotation is wrong (wrong tag or missing entirely).

3. **PlusCal/TLA+ desync**: TSXSGL, TL2, SwissTM, LEFTRIGHT, XTM, Romulus all have `lastFence` only in the TLA+ translation, not in the PlusCal source (documented with NOTE comments). This means re-translating from PlusCal would lose the annotations.

4. **C++ vs Rust architectural divergence (critical finding)**: Three backends have Rust ports that are architecturally different from both the C++ and the TLA+ model:
   - **TL2**: Rust adds global commit lock + `fence(SeqCst)` at 6 points — not in C++ or model
   - **Romulus**: Rust has NO read-set, NO lock-bit phase, adds `fence(SeqCst)` at 4 points
   - **XTM**: Rust is version-table OCC (different algorithm from C++ page-granularity)

5. **NVHTM is the worst offender**: The TLA+ model describes checkpoint/recovery protocol + SGL fallback that don't exist in C++. The C++ has pass-through mode on RTM failure, which the model replaces with SGL. This is the only backend where the model describes a fundamentally different algorithm from both C++ and Rust.

6. **TinySTM family best fidelity**: Despite the double-check abstraction and signal-vs-thread-fence gap, the TinySTM models capture the core algorithm correctly (endVersion, L_extend, split commit phases, FenceFidelity). All three backends have accurate lock encoding and state machines.

7. **Rust `fence(SeqCst)` vs C++ `atomic_signal_fence(seq_cst)`**: Rust uses `fence(SeqCst)` at commit time (CPU barrier), C++ uses only `atomic_signal_fence(seq_cst)` (compiler barrier). This means Rust produces strictly stronger ordering at commit. The `lastFence` model cannot distinguish these.

8. **ARM vulnerability (LEFTRIGHT)**: The LEFTRIGHT read path does a plain `read_value_from_addr` before `get_clock()` acquire-load. On ARM, the plain load CAN be reordered after the acquire, creating a correctness bug. The TLA+ model treats both as an atomic step and cannot detect this.

## Recommended Model Improvements

1. **NVHTM model rewrite**: Model describes checkpoint/recovery and SGL fallback that don't exist in C++. C++ uses pass-through on RTM failure. Requires a rewrite of ~300 lines.

2. **Model Rust backends separately**: Rust TL2 and Romulus have architectural differences from C++ (Rust TL2 lacks write-set sort; Rust Romulus lacks read-set validation entirely). Would require new TLA+ models.

3. **PersistentSGL C++ fence**: Model correctly splits dual-write into `L_active` (mem) + `L_write_nvm` (nvm) with crash window. C++ still has no fence between `*addr=val` and `memcpy` — model reveals this correctly. The C++ fix is to add `atomic_thread_fence(seq_cst)` before the mmap write.

## Bugs Found by TLC

(Unchanged from previous audit — see original SUMMARY.md for 17 bugs found across all 19 backends.)

## Recommended C++ Implementation Fixes

1. **PersistentSGL dual-write durability** (`PersistentSGL_runtime.cpp:96-136`): Add `atomic_thread_fence(seq_cst)` or `msync(MS_SYNC)` between the memory write and the mmap write to ensure crash consistency. Without this, a crash between `*addr=val` and `memcpy` leaves the persistent file with stale data.

2. **NVHTM SGL fallback**: Consider adding a proper SGL fallback (matching SPHT fix from 2026-06-20) instead of pass-through mode. The current pass-through on RTM failure provides no TM guarantees at all.
