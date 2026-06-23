# Audit: SimEngine (Discrete-Event Simulation Engine)

**Score: 2/5** — The TLA+ spec is named "SimEngine" but actually models the DES `SimState` (`engine.rs`) conflict-resolution protocol, not the real-backend replay engine (`SimEngine` in `sim_engine.rs`). The naming mismatch obscures a fundamental gap: the spec covers only the abstract conflict-resolution layer, while the implementation has two entirely separate engines with different architectures, clock modes, address translation, backend dispatch, and correctness verification. The spec captures WAR/RAW resolution well but misses cost mode, address translation, checkpoint/restore, deadlock detection, and the full event-type dispatch.

## Files

| Artifact | Path | Lines | Role |
|----------|------|-------|------|
| TLA+ spec | `docs/proofs/SimEngine.tla` | 304 | Cross-LP conflict resolution protocol (models `engine.rs`) |
| TLC config (parallel) | `docs/proofs/SimEngine.cfg` | 15 | LP={0,1}, Addr={0,1}, 8 invariants |
| TLC config (sequential) | `docs/proofs/SimEngine-sequential.cfg` | 15 | LP={0}, Addr={0} |
| Trace replay engine | `simulator/src/sim_engine.rs` | 686 | Real-backend trace replayer (cost mode + timestamp mode) |
| DES engine | `simulator/src/engine.rs` | 357 | Pure DES engine: SimState, conflict detection, cost model |
| Backend dispatch | `simulator/src/backend.rs` | 775 | Backend 6-way dispatch (Norec/Tl2/Tinystm/Romulus/Swisstm/TsxSim) |
| Event types | `simulator/src/event.rs` | 77 | Event/EventKind definitions |
| Verifier | `simulator/src/verifier.rs` | 500 | Shadow memory + correctness checks |
| Computation profile | `simulator/src/computation_profile.rs` | 57 | Baseline wall-time parser |
| Trace spec (auto-gen) | `docs/proofs/SimEngine_TTrace_1782238158.tla` | 175 | TLC trace replay from earlier model-checking run |

## Algorithm Summary

The TLA+ spec (`SimEngine.tla`) models a cross-LP conflict-resolution protocol for transactional memory: in-flight writes and reads are tracked per-LP; on a Write(addr), any older LP reading that address is aborted (RAW); on a Read(addr), any older LP writing that address is aborted (WAR). The "newest operation wins" rule resolves all conflicts. SGL fallback provides mutual exclusion when enabled. The Rust implementation has **two separate engines**: `SimState` (`engine.rs`) — a pure DES engine that implements exactly this conflict protocol with cost/timestamp clock modes and shadow memory; and `SimEngine` (`sim_engine.rs`) — a real-backend trace replayer that runs actual TM backends (NOrec, TL2, TinySTM, etc.) through event traces, detecting true aborts from the backend rather than synthetic conflict detection.

## Cross-Reference Checklist

### Engine A: DES SimState (`engine.rs`) — matches TLA+ spec

| Rust `engine.rs` / `SimState` | TLA+ label/action | Match | Notes |
|---|---|---|---|
| `in_flight_writes: HashMap<u32, Vec<u64>>` | `in_flight_writes: set of <<lp, addr>>` | ✅ | Same tracking structure |
| `in_flight_reads: HashMap<u32, Vec<u64>>` | `in_flight_reads: set of <<lp, addr>>` | ✅ | Same tracking structure |
| `lp.in_tx` field | `in_tx[lp]` | ✅ | Per-LP in-transaction flag |
| `dispatch(): TxBegin` → `lp.in_tx = true` | `BeginTx(lp)` | ✅ | Begin sets in_tx, pc → "active" |
| `dispatch(): TxEnd` → `lp.in_tx = false; untrack(tid)` | `CommitTx(lp)` | ✅ | Clears in-flight, increments committed count |
| `dispatch(): Abort` → `lp.in_tx = false; untrack(tid)` | `AbortTx(lp)` | ✅ | Clears in-flight, increments abort count |
| `track_write(lp_id, addr)` → push to `in_flight_writes[lp_id]` | `WriteAddr(lp, a)` — add `<<lp,a>>` to in_flight_writes | ✅ | Same union semantics |
| `track_read(lp_id, addr)` → push to `in_flight_reads[lp_id]` | `ReadAddr(lp, a)` — add `<<lp,a>>` to in_flight_reads | ✅ | Same union semantics |
| `check_raw_conflict(lp_id, addr)` → scan readers | `FindReader(lp, a)` | ✅ | WAR: writer scans for readers |
| `check_war_conflict(lp_id, addr)` → scan writers | `FindWriter(lp, a)` | ✅ | RAW: reader scans for writers |
| `abort_lp(victim)` → untrack + increment conflict_aborts | `ConflictAbort(lp)` / conflict_aborts' increment | ✅ | Aborts older LP, charges abort cost |
| **Cost clock mode**: `clock += event_cost` | Not modeled | ❌ | TLA+ has no cycle costs |
| `ConfComputation { cycles }` handling | Not modeled | ❌ | Computation events ignored in spec |
| `retry_cost_multiplier` on abort penalty | Not modeled | ❌ | Retry cost multiplier not in spec |
| `ShadowMemory` alloc/free | `Alloc`/`Free` events dispatch | ❌ | No memory model in spec |
| `Checker` invariant checks | Not modeled | ❌ | No shadow checker in spec |
| SGL fallback in `SimState` | Not present | ❌ | `SimState` has no SGL field; only `SimEngine` has SGL |
| `MachineProfile` / `BackendProfile` | Not modeled | ❌ | No hardware profile model |
| `CalibratedCostModel` | Not modeled | ❌ | Cost model is purely abstract |

### Engine B: Real-backend SimEngine (`sim_engine.rs`) — NOT modeled by TLA+

| Rust `sim_engine.rs` / `SimEngine` | TLA+ equivalent | Match | Notes |
|---|---|---|---|
| `backend: Backend` (real TM backend dispatch) | Not modeled | ❌ | TLA+ has no concept of real backends |
| `dispatch_event()` → calls `b.begin()`, `b.commit()`, etc. | Not modeled | ❌ | Real backend calls abstracted away |
| `process_event()` — main event loop with cost accumulation | Not modeled | ❌ | TLA+ has no cost or time |
| Cost mode: `estimated_cycles += event_cost()` | Not modeled | ❌ | Purely a Rust feature |
| Address translation: `translate_addr(trace_addr) = trace_addr + addr_addend` | Not modeled | ❌ | TLA+ has fixed Addr set |
| `flush_pending_begins()` — TSX retry loop round-robin | Not modeled | ❌ | TLA+ BeginTx is a single-step action |
| `sgl_mode: HashMap<u64, bool>` per-thread (cost accounting only) | `sgl_mode[lp]` (mutual exclusion model) | ⚠️ Partial | TLA+ models SGL as full mutual exclusion; SimEngine only tracks it for cost accounting |
| `ensure_thread()` / `seen_threads` | Not modeled | ❌ | Thread lifecycle not in spec |
| `checkpoint::snapshot_engine()` / `restore()` | Not modeled | ❌ | Checkpoint/restore not in spec |
| `deadlock::DeadlockDetector` | `Progress` (liveness) | ⚠️ Partial | TLA+ has `Progress` temporal property but no deadlock cycle detection |
| `verifier::Verifier` — shadow memory + use-after-free | Not modeled | ❌ | No verifier equivalent in spec |
| `Computation { cycles }` handling | Not modeled | ❌ | Computation events not in spec |
| `ThreadSpawn` / `ThreadJoin` | Not modeled | ❌ | TLA+ has static LP set |
| `Alloc` / `Free` events | Not modeled | ❌ | No allocation model in spec |
| `Assert` / `Log` / `Checkpoint` events | Not modeled | ❌ | Auxiliary events not modeled |
| `TmxAbort` catch_unwind handling (read/write abort) | Not modeled | ❌ | Backend-triggered aborts not in spec |

### Backend dispatch (`backend.rs`)

| Rust `backend.rs` | TLA+ equivalent | Match | Notes |
|---|---|---|---|
| `Backend` enum: Norec, Tl2, Tinystm, Romulus, Swisstm, TsxSim | One abstract LP model | ❌ | No per-backend model; TLA+ abstracts all backends as one protocol |
| `begin()`, `commit()`, `abort()` | BeginTx, CommitTx, AbortTx | ✅ | Same lifecycle |
| `try_begin()` / `force_sgl()` | Not modeled | ❌ | TSX-specific retry not in spec |
| `sim_set_thread_id()` / `sim_clear_thread_id()` | Not modeled | ❌ | Simulation threading not in spec |
| `read_u8/16/32/64()` `write_u8/16/32/64()` | ReadAddr, WriteAddr | ✅ | Width-specific read/write abstracted to single addr |
| `sim_snapshot_bytes()` / `sim_restore_bytes()` | Not modeled | ❌ | Checkpoint not in spec |
| `take_stats()` / `print_stats()` | Not modeled | ❌ | Statistics not in spec |

## Invariants

| Invariant | TLA+ label | Checked by TLC? | Meaning | Sequential (LP={0}) | Parallel (LP={0,1}) |
|---|---|---|---|---|---|
| `NoConcurrentWrites` | I1 (line 238) | ✅ | An address can be written by at most one LP at a time | ✅ PASS | ✅ PASS |
| `SGLMutex` | I2 (line 243) | ✅ | At most one LP in SGL mode at any time | ✅ PASS | ✅ PASS |
| `SGLIsolation` | I3 (line 248) | ✅ | SGL-mode LP blocks all others' in-flight ops | ✅ PASS | ✅ PASS |
| `NoOrphanedOps` | I4 (line 256) | ✅ | LP not in TX has no in-flight writes/reads | ✅ PASS | ✅ PASS |
| `SGLStateConsistent` | I5 (line 262) | ✅ | sgl_mode[lp] => pc[lp] = "sgl" | ✅ PASS | ✅ PASS |
| `WriteTrackingConsistent` | I6 (line 267) | ✅ | In-flight writes => LP in_tx and active/sgl | ✅ PASS | ✅ PASS |
| `ReadTrackingConsistent` | I7 (line 272) | ✅ | In-flight reads => LP in_tx and active/sgl | ✅ PASS | ✅ PASS |
| `NoSelfConflict` | I8 (line 279) | ✅ | No LP reads and writes same address simultaneously | ✅ PASS | ✅ PASS |
| `Progress` (liveness) | Temporal (line 288) | ❌ Not in config | Every active LP eventually becomes idle | Not checked | Not checked |
| `NotAllAborted` (liveness) | Temporal (line 295) | ❌ Not in config | Not all LPs are permanently aborted | Not checked | Not checked |

**TLC model checking results:**

- **Parallel** (LP={0,1}, Addr={0,1}): **8 states generated, 5 distinct, 0 deadlock, all invariants PASS** ✅
- **Sequential** (LP={0}, Addr={0}): **5 states generated, 3 distinct, 0 deadlock, all invariants PASS** ✅

**Note on uncovered invariants**: `NoDirtyRead` (defined as `NoDirtyRead` concept but not named in spec — the spec has no equivalent of TL2's lock-bit validation gap) is not present. The `Progress` and `NotAllAborted` temporal properties are defined at lines 288–296 but **not included in the TLC config**. Temporal model checking would require `PROPERTY Progress NotAllAborted` in the config, which may cause state explosion with LP={0,1}.

## Deviations

### 1. Naming mismatch: spec models `engine.rs`, not `sim_engine.rs` (Critical)

**Issue**: The file is named `SimEngine.tla` and the module is `SimEngine`, which strongly implies it models the `SimEngine` struct in `sim_engine.rs`. But the spec's variables (`in_flight_writes`, `in_flight_reads`, `in_tx`, etc.) directly match the `SimState` struct in `engine.rs`. The real `SimEngine` in `sim_engine.rs` has no `in_flight_writes` or conflict detection — it delegates all TM operations to real backends and only tracks `in_tx` per thread for the verifier.

**Impact**: Any reader expecting the TLA+ spec to describe the real-backend replay engine will be confused. The spec describes a pure DES protocol that does not exist in the codebase under the name "SimEngine" — it exists as `SimState` in `engine.rs`.

**Recommendation**: Rename the TLA+ module to `DESEngine` or `SimStateConflictProtocol`. Update the module header comment to clearly state: "This spec models the pure DES engine (SimState in engine.rs), NOT the real-backend replay engine (SimEngine in sim_engine.rs)."

### 2. Cost mode and cycle accounting entirely unmodeled (High)

**Issue**: The most architecturally significant feature of both engines — cost-accumulation clock mode — has no TLA+ representation. The spec lacks cycle costs, `CalibratedCostModel`, `MachineProfile`, `computed_cycles`, `estimated_cycles`, and frequency conversion.

**Impact**: The TLA+ spec cannot validate any timing-related behavior. Since cost mode is the primary purpose of the simulation infrastructure ("what-if analysis for different hardware/backends"), this is a large gap.

**Recommendation**: Add a `cycles` variable to the TLA+ model and define `CostRead`, `CostWrite`, `CostBegin`, `CostCommit`, `CostAbort` constants. Add invariant `CostNonNegative` to verify monotonic clock advancement.

### 3. Address translation not modeled (High)

**Issue**: `SimEngine::init_at()` computes `addr_addend = mapped_base - trace_base` and applies it via `translate_addr()` on every read/write event. The TLA+ spec has a static `Addr` constant set with no translation concept.

**Impact**: The address-space mapping between trace events and backend memory regions is invisible in the spec. This means a class of bugs (address overflow, mmap collision with process segments) is not covered.

**Recommendation**: Add `addr_addend: Int` variable and `TranslateAddr(a) == a + addr_addend` operator to the spec. Model mmap as an init action that sets the addend.

### 4. Checkpoint/restore not modeled (Medium)

**Issue**: `SimEngine` has full `checkpoint::snapshot_engine()` and `restore()` support with bincode-serialized backend state. The TLA+ spec has no snapshot/restore semantics.

**Impact**: No formal verification of checkpoint consistency — e.g., that restoring a snapshot preserves the invariants, or that serialization round-trips correctly.

**Recommendation**: Add `Checkpoint` action that records the full state tuple; add `Restore` action that verifies the restored state matches a prior Checkpoint. TLC can check idempotency.

### 5. Deadlock/livelock detection not modeled (Medium)

**Issue**: `SimEngine` has a `DeadlockDetector` that checks for livelock cycles (thread A waiting for thread B's address). The TLA+ spec defines `Progress` and `NotAllAborted` temporal properties but does not include them in the TLC config, and there is no cycle-detection logic.

**Impact**: Livelock scenarios where two threads mutually abort each other's transactions indefinitely are not checked by TLC.

**Recommendation**: Add `PROPERTY Progress NotAllAborted` to the TLC config (at least for the sequential model where state explosion is minimal). Consider adding a concrete `CycleDetected` invariant for the parallel model.

### 6. TSX retry loop (Phase 4) not modeled (Medium)

**Issue**: `SimEngine::flush_pending_begins()` implements a round-robin retry loop for the TSX-sim backend: up to 5 retry rounds, then force SGL fallback. The TLA+ spec's `BeginTx` is a single-step unconditional action.

**Impact**: The TSX retry semantics (lock-busy detection, xbegin_cost accumulation, SGL fallback after max_r rounds) are completely invisible in the spec. Bugs in the retry logic (e.g., incorrect round count, cost double-charge) cannot be found.

**Recommendation**: Add `retry_count[lp]` variable, `TryBegin(lp)` action (non-deterministic success/failure), and `ForceSGL(lp)` action that transitions to `sgl` state when `retry_count[lp] >= max_r`.

### 7. Thread lifecycle and dynamic creation not modeled (Medium)

**Issue**: `SimEngine::ensure_thread()` lazily initializes threads when first seen; `EventKind::ThreadSpawn(child_id)` creates new threads. The TLA+ spec has a static `LP` constant set.

**Impact**: No verification of thread-initialization ordering (e.g., that a thread is initialized before its first TM operation).

**Recommendation**: Model threads as dynamically created via `SpawnThread(child_id)` action that adds to the LP set. This requires transitioning from a constant set to a variable set of LP IDs.

### 8. Verifier correctness checks not modeled (Low)

**Issue**: The `Verifier` in `sim_engine.rs` detects use-after-free, double-free, commit-without-begin, abort-without-begin, and money conservation violations. None of these are in the TLA+ spec.

**Impact**: Memory safety and API-correctness properties are not formally verified.

**Recommendation**: Add `shadow_alloc[lp, addr]` and `tx_depth[lp]` variables to the spec. Define invariants: `NoUseAfterFree`, `NoCommitWithoutBegin`, `NoAbortWithoutBegin`, `MoneyConserved`.

### 9. SGL fallback semantics mismatch (Low)

**Issue**: In the TLA+ spec, `EnterSGL` requires `\A other : ~IsWriting(other, a) /\ ~IsReading(other, a)` — full mutual exclusion. In `sim_engine.rs`, `sgl_mode` is only used for cost accounting (swapping `tx_end_cost` for `sgl_end_cost`). The real backend's SGL behavior is opaque — the SimEngine never actually acquires a mutex.

**Impact**: The TLA+ model is more strict than the implementation. If a bug in the real backend's SGL fallback allowed concurrent access, the spec would not catch it because the SimEngine doesn't enforce SGL isolation at the protocol level.

**Recommendation**: Either add a `force_sgl()` call to the SimEngine's dispatch path (actually acquiring a mutex in SimEngine for all threads when sgl_mode is true), or remove the SGL tracking entirely from the SimEngine and document that SGL fallback is purely a cost-model concern.

### 10. Computation events direct-credited without backend dispatch (Low)

**Issue**: `SimEngine::process_event()` handles `EventKind::Computation { cycles }` by adding `cycles` to both `computed_cycles` and `estimated_cycles` and returning early without dispatching to the backend. This means computation events never trigger conflict detection or backend reads/writes.

**Impact**: If a Computation event should interact with the TM state (e.g., non-TM computation that accesses shared memory), it would be invisible to the simulation. The spec has no way to validate this.

**Recommendation**: Add a `Computation` action to the TLA+ spec, even if it simply advances the clock and leaves state unchanged. Document the rationale (Computation = pure non-TM work, by definition has no memory interaction).

### 11. `EventKind::Assert`, `Log`, `Checkpoint`, `ThreadJoin` not modeled (Low)

**Issue**: These four event kinds are dispatched in `dispatch_event()` but have no TLA+ representation. `Checkpoint` triggers watermark advancement in the DES engine; `Assert` reports failures; `Log` prints messages; `ThreadJoin` is a no-op.

**Impact**: Minor — these are auxiliary events with no correctness impact on TM semantics.

### 12. Backend-specific simulation features not modeled (Low)

**Issue**: `backend.rs` exposes backend-specific APIs (`is_tsx_sim()`, `try_begin()`, `force_sgl()`, `sim_snapshot_bytes()`, `sim_restore_bytes()`, `take_stats()`, `print_stats()`) that have no TLA+ equivalent.

**Impact**: Each backend may have simulation-specific semantics (e.g., TsxSim's bloom-filter conflict detection) that are invisible at the TLA+ level.

## Summary

| Dimension | Finding | Risk | Priority |
|-----------|---------|------|----------|
| **Spec vs Implementation** | TLA+ spec models `engine.rs` (SimState), not `sim_engine.rs` (SimEngine) | High — naming mismatch causes confusion | P0 |
| **Invariant coverage** | 8/8 invariants pass TLC; temporal properties (Progress, NotAllAborted) not in config | Medium — liveness unchecked | P1 |
| **Cost mode** | Not modeled — spec is purely logical | High — cost mode is primary purpose | P1 |
| **Address translation** | Not modeled — fixed Addr set only | High — mmap collision bugs invisible | P1 |
| **TSX retry loop** | Not modeled — BeginTx is single-step | Medium — retry bug detection gap | P2 |
| **Checkpoint/restore** | Not modeled | Medium — consistency gap | P2 |
| **Deadlock detection** | Temporal properties defined but not in config | Medium — livelock unchecked | P2 |
| **Thread lifecycle** | Static LP set — no spawn/join | Medium — init ordering unchecked | P2 |
| **Verifier checks** | No shadow memory, use-after-free, money conservation | Low — protocol-level spec | P3 |
| **SGL semantics** | Spec stricter than implementation (mutual exclusion vs cost accounting) | Low — cost-only SGL works correctly | P3 |
| **Backend dispatch** | 6 backend variants abstracted to one protocol | Low — protocol is backend-agnostic by design | P3 |
| **Auxiliary events** | Assert/Log/Checkpoint/ThreadJoin not modeled | Low — no correctness impact | P3 |
| **TLC coverage** | 5 states (sequential), 8 states (parallel) — small model | Low — protocol simple enough | Info |
