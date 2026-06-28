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
| **PersistentSGL** | **2/5** | 1/5 | **Downgraded**. No `lastFence` tracking. Simultaneous dual-write (`mem[a]:=v ∧ nvm[a]:=v` in one action) hides real durability gap: C++ does `*addr=val` then `memcpy` to mmap with NO fence between; `msync` only at process exit. `NVMAgreesWithMem` invariant holds in model but NOT in C++. Bump allocator `__atomic_fetch_add(relaxed)` not modeled. | High |

### Phase 2 Backends (PlusCal)

| Backend | Overall | MO | Key gaps | Risk |
|---------|---------|----|----------|------|
| **Romulus** | **3/5** | 2/5 | **Downgraded**. `lastFence` present but TWO critical `atomic_thread_fence(seq_cst)` calls have NO annotation in model (line 226 after lock-bit set, line 236 after write-back). Lock-bit phase `fetch_or(acq_rel)` has NO fence annotation. Version table `store(release)` has NO annotation. Rust backend is architecturally different (no read-set, no lock-bit phase, adds `fence(SeqCst)` at 6 extra points). | Medium |
| **TL2** | **3/5** | 2/5 | **Downgraded**. `lastFence` present but C++ clock increment is `fetch_add(relaxed)` while model annotates it `"sc"` — fundamentally wrong ordering. Rust backend has architecturally different global commit lock + `fence(SeqCst)` barriers + read-validate loop not present in C++ or model. Guard-table double-check protocol abstracted. | Medium |
| **XTM** | **3/5** | 2/5 | **Downgraded**. `lastFence` present but assignments are inaccurate: `"sc"` where C++ uses `load(acquire)`, `"rel"` where C++ uses `fetch_add(acq_rel)` (loses acquire half). Bloom filter (`g_xf[]`) entirely absent (optimization only). Rust backend completely different algorithm (version-table OCC, not page-granularity). | Medium |
| **LEFTRIGHT** | **3/5** | 2/5 | **Downgraded**. `lastFence` present but write path has ZERO ordering operations in C++ while model annotates `"acq"` — wrong direction of over-estimation. Read-path data-race vulnerability on ARM (plain `read_value_from_addr` before `get_clock()` acquire-load — CPU can reorder data read after acquire). Value-based validation `memcmp` ordering abstracted. Queue-mode bypass not modeled. | Medium |
| **SwissTM** | **3/5** | 2/5 | **Downgraded**. `lastFence` present but `r_lock.exchange(acq_rel)` in commit Phase 1 modeled as `"acq"` only — missing the release side that makes write-back visible to readers. Contention manager ordering (`greedy_ts` fetch_add, `cm_ts` acquire) not modeled. Undo-log restore ordering on abort not captured. `write_impl` `signal_fence(seq_cst)` missing from model. | Medium |

### Phase 3 Backends (TLA+-only / PlusCal)

| Backend | Overall | MO | Key gaps | Risk |
|---------|---------|----|----------|------|
| **NOrec** | **4/5** | 4/5 | **Updated 2026-06-28: lastFence[t] tracking added** — all 5 ordering operations annotated (acq: get_clock, sc: CAS commit, rel: set_clock). FenceFidelity checks pc=L_commit_wb => lastFence ∈ {"sc","rel"}. Torn-read double-check split (L_active→L_read_val). Remaining minor gaps: acquire/release as annotation only, plugin-mode bypass not modeled. | Low |
| **DUDETM** | 1/5 | 1/5 | Unchanged. Zero memory ordering captured from 30+ C++ atomic operations or 8+ Rust fences. Model is high-level design sketch. | High |
| **NVHTM** | **1/5** | 1/5 | **Downgraded (critical)**. Model describes an algorithm that DOES NOT EXIST in C++: checkpoint/recovery protocol (`L_write_cp`, `L_apply_log`, `L_clear_cp`) with SGL fallback (`L_active_sgl`). C++ NVHTM has NO checkpoint protocol, NO SGL fallback, and uses pass-through mode on RTM failure. 12 `lastFence` annotations mostly correspond to nothing in C++. Redo log is fixed-size array in C++, unbounded sequence in model. | Critical |
| **SPHT** | 2/5 | 1/5 | Unchanged. `_mm_sfence()` and `_mm_clflush()` for NVM durability not modeled. RTM retry logic differs (single attempt → SGL in C++; multiple retries → SGL in model). `g_durable_seqs` release store absent. Crash/recovery modeled but absent in C++. No `lastFence` tracking. | High |
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

1. **Add `lastFence` to remaining 8 backends**: SGL, PersistentSGL, DistributedSGL, SPHT, TiKV, TSXSim, DESEngine, DUDETM. NOrec completed 2026-06-28.

2. **Split `lastFence` into two variables**: `lastSignalFence[t]` for compiler-only barriers and `lastThreadFence[t]` for CPU barriers. This would let `FenceFidelity` check that the right TYPE of fence was emitted at each label.

3. **Add RMW ordering annotation**: For operations like `fetch_add(acq_rel)`, `exchange(acq_rel)`, `fetch_or(acq_rel)`, add a separate `lastRmw[t]` with value `"acq_rel"` or `"relaxed"` to capture bundled ordering.

4. **Fix NVHTM model**: Remove checkpoint/recovery protocol and SGL fallback. Add pass-through mode on RTM failure. Add `_mm_sfence`/`_mm_clflush` model. This requires a rewrite.

5. **Model Rust backends separately**: The Rust TL2 and Romulus backends have architectural differences significant enough to warrant their own TLA+ models. The Rust Romulus lacks read-set validation entirely — this is a correctness gap, not just a modeling abstraction.

6. **PersistentSGL dual-write fix**: Add `atomic_signal_fence(seq_cst)` between `*addr=val` and `persist_write` in C++, or split the model write into two separate actions with a crash-possible intermediate state.

## Bugs Found by TLC

(Unchanged from previous audit — see original SUMMARY.md for 17 bugs found across all 19 backends.)

## Recommended C++ Implementation Fixes

1. **LEFTRIGHT read data before acquire-load** (`leftright.hpp:285-296`): On ARM, the plain `read_value_from_addr` before `get_clock()` acquire-load can be reordered. Either use `atomic_thread_fence(acquire)` between data read and clock capture, or use `__atomic_load_n(acquire)` for the data read.

2. **TL2 clock increment** (`tl2.hpp:232`): `g_clock.fetch_add(1, relaxed)` should be `memory_order_release` to ensure write-back stores are visible before the clock advance. The Rust version already uses `Release`.

3. **PersistentSGL dual-write durability** (`PersistentSGL_runtime.cpp:96-136`): Add `atomic_thread_fence(seq_cst)` or `msync(MS_SYNC)` between the memory write and the mmap write to ensure crash consistency. Without this, a crash between `*addr=val` and `memcpy` leaves the persistent file with stale data.

4. **NVHTM SGL fallback**: Consider adding a proper SGL fallback (matching SPHT fix from 2026-06-20) instead of pass-through mode. The current pass-through on RTM failure provides no TM guarantees at all.
