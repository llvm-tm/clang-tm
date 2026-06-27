# Audit Plan: TLA+ Specs vs C++ Implementations

## Goal

For each of the 18 TM backends, verify that the TLA+ model faithfully represents the C++ implementation's TM semantics. Identify any abstraction gaps that could hide real bugs.

## Methodology

Each backend audit follows a 4-step process:

### Step 1: Semantic abstraction identification

For each backend, document what the TLA+ model **abstracts away** vs the C++ implementation:

| Abstraction | Typical examples |
|------------|------------------|
| **Data values** | All backends model `Data` as a small set (`{0}` or `{0,1}`). Real implementations handle arbitrary 8/16/32/64-bit values. |
| **Address space** | All backends model `Addr` as a small set (`{0}` or `{0,1}`). Real implementations handle 64-bit address ranges with mmap'd TM regions. |
| **Memory model** | TLA+ assumes single-copy atomicity (sequential consistency). Real implementations have relaxed memory ordering with `atomic_signal_fence`/`atomic_thread_fence`. |
| **Thread count** | All backends check 2 threads. Real implementations scale to 64+ threads. |
| **Retry loops** | `siglongjmp`-based retry is replaced with direct `goto` to L_idle. |
| **Allocation** | Region allocator (mmap, free-list) is not modeled — all data is pre-allocated in `Addr`. |
| **STM-internal malloc** | `tm_calloc`/`tm_malloc` calls during transactions are not modeled. |
| **Crash recovery** | Only PersistentSGL and DistributedSGL model crash/recovery. Others assume durable memory. |
| **Contention management** | Backoff, exponential backoff, and adaptive retry are not modeled. |
| **Bloom filters** | TSXSGL's hardware read-set tracking is modeled as `readSet = Addr` (full address set). |
| **Lock-free CAS** | NOrec's CAS on global clock is modeled as atomic increment (no spurious CAS failures). |

### Step 2: Per-backend abstraction gaps

For each backend, specific gaps between the TLA+ model and C++ code:

#### Phase 1 backends (PlusCal, verified)

| Backend | Abstraction gaps | Risk level |
|---------|-----------------|------------|
| **SGL** | No gaps — lock+mem is the entire implementation. Read-set/write-set/version are proof scaffolding, not runtime state. | Low |
| **TSXSGL** | Model assumes TSX captures the FULL address space (`readSet = Addr` at `xbegin`). Real TSX has cache-line granularity with eviction (capacity aborts). SGL fallback uses `std::mutex::lock()` which can block (model assumes `lock=0` is simple boolean). Retry counter modeled as `MaxRetries` bound — real TSX uses `_xbegin()` with hardware abort handling. | Medium — capacity abort path missing could hide TSX→SGL fallback bugs. |
| **TinySTM_WBCTL** | Model locks all written addresses atomically; real implementation uses per-location locking with sorted order. Write-back is a single atomic step in model; real implementation loops over write-set. | Low — lock ordering doesn't affect safety (both ensure mutual exclusion). |
| **TinySTM_WBETL** | Same as WBCTL. Encounter-time locking means locks acquired during write, not at commit — model captures this by setting lock at write time. | Low |
| **TinySTM_WT** | Write-through semantics (write to memory immediately, undo log for abort) matches model exactly. Undo log not modeled as separate data structure — abort just resets `mem` to pre-transaction values (implicit undo). | Low |
| **PersistentSGL** | `durable_log` modeled as per-thread set of executed operations. Real persistent memory has failure-atomicity constraints (cache-line flush ordering, clwb/fence). Model assumes synchronous durability; real PM requires `pmem_persist()` or equivalent. | Medium — crash recovery sequence in model matches the C++ `recover()` function, but PM write ordering not enforced. |

#### Phase 2 backends (PlusCal, planned)

| Backend | Abstraction gaps | Risk level |
|---------|-----------------|------------|
| **TL2** | Lock acquisition is atomic in model (`guard` update on all written addresses at once). Real TL2 acquires locks one at a time in sorted order. Validation in model is atomic read-set scan; real TL2 may interleave. | Medium — sorted lock acquisition prevents deadlock in real impl but model doesn't capture ordering. |
| **Romulus** | ✅ Verrified with TLC (141K states). Version table OCC protocol matches C++: acquire lock → validate → set lock bits → inc clock → write back → update version → release lock. Read-validate omitted (PlusCal atomicity makes re-check unnecessary). Commit lock spin-loop not modeled (model is `lock=0 → lock=self`). | Low — spin-loop is a performance optimization, not a correctness concern. |

#### Phase 3 backends (TLA+-only, no PlusCal)

| Backend | Abstraction gaps | Risk level |
|---------|-----------------|------------|
| **NOrec** | CAS on global clock is atomic compare-and-swap in C++ but simple increment in TLA+. Value-based validation (re-read and compare) is not modeled — validation only checks version numbers. | Medium — CAS semantics could fail where simple increment succeeds. |
| **DUDETM** | 3-phase commit + concurrent flush. Model captures phases but not the concurrent flush overlap (PlusCal sequentializes it). | Medium |
| **NVHTM** | RTM conflict semantics are non-deterministic in hardware (abort reason encoding). Model simplifies to simple boolean (abort/not abort). | High — hardware semantics are inherently non-deterministic and the model may miss abort-reason-dependent paths. |
| **SPHT** | Group commit batches multiple transactions. Model sequentializes them. | High — group-commit overlap creates interleavings not captured. |
| **DistributedSGL** | Message passing is non-deterministic delivery. Model assumes synchronous communication. | High — network reordering/duplication not captured. |
| **TiKV** | Percolator 2PC with per-key locks. Network failures, timeouts, and async RPCs not captured. | High — distributed system semantics dominate. |
| **TSXSim** | Simulation engine (bloom filter + capacity tracking). Not a TM algorithm — correctness depends on simulator calibration, not protocol. | Low (not a correctness spec) |
| **SimEngine** | Trace-driven simulation. Not a TM algorithm. | Low (not a correctness spec) |

### Step 3: Cross-validation test

For each backend, run a cross-validation test:

1. **Construct** a minimal C++ test program exercising the backend paths that the TLA+ model checks (acquire lock, validate, write-back, abort).
2. **Instrument** the C++ code with `printf` at each transition (begin, read, write, commit, abort).
3. **Generate** a trace of C++ execution interleavings (using thread-sanitizer or manual analysis).
4. **Compare** against TLC-generated state graph: every valid C++ execution trace should correspond to a path in the TLC state graph.

### Step 4: Invariant verification cross-check

For each invariant proved by TLC model checking:

| Invariant | C++ check method | Expected |
|-----------|-----------------|----------|
| `LockExclusion` | Assert that `lock == 0` before `lock = self`; assert `lock == self` in commit phases. | Runtime assertion |
| `NoDirtyRead` | Assert that read-set entries were not modified between read and commit. | Assertion in `validate()` |
| `ClockMonotonic` | Assert `new_clock > old_clock` on increment. | Runtime check |
| `ReadSetConsistent` | Re-read version at commit time, assert unchanged. | Core OCC invariant |

## Priority ordering

Audit backends in order of:
1. **Romulus** (already PlusCal, minimal gaps) — audit first as calibration
2. **SGL, TSXSGL, TinySTM_WBCTL/WBETL/WT, PersistentSGL** (Phase 1, PlusCal)
3. **TL2, XTM, LEFTRIGHT, SwissTM** (Phase 2, PlusCal planned)
4. **NOrec, DUDETM, NVHTM, SPHT** (Phase 3, TLA+-only)
5. **DistributedSGL, TiKV** (Phase 3, distributed)

## Concrete audit steps

For each backend:

### 1. Read C++ source
- Identify all atomic operations (`atomic<T>::store/load/exchange/CAS`)
- Identify all fences and memory ordering constraints
- Identify all lock acquisition/release points
- Identify all read-set/write-set operations
- Identify abort paths and retry logic

### 2. Cross-reference with TLA+ model
- For each atomic step in the C++ code, find the corresponding TLA+ action
- Verify that the TLA+ action's variable updates match C++ assignments
- Check that C++ fences/ordering are represented (as atomicity in TLA+)
- Verify that all C++ control flow paths have matching TLA+ transitions

### 3. Document deviations
- Any C++ operation not represented in TLA+ (abstraction)
- Any TLA+ action not corresponding to a real C++ step (idealization)
- Any C++ guard condition simplified in TLA+

### 4. Create audit report per backend
- Score: 1-5 (5 = perfect match, 1 = major abstraction gaps)
- List of deviations with risk assessment
- Recommended model improvements (if any)

## Example: Romulus audit

**Score: 4/5**

**Perfectly modeled:**
- Begin (snapshot clock, clear sets)
- Read (return buffered if written, capture version, record in read-set)
- Write (buffer in write-set, set read_only = false)
- Acquire commit lock
- Validate (WS version ≤ timestamp, RS version unchanged)
- Set lock bits on version entries
- Increment global clock
- Write-back buffered values to memory
- Update version entries with commit_ts
- Release commit lock
- Read-only commit
- Abort (clean up, increment abort counter)

**Deviations:**
1. **Commit lock spin-loop (Medium)**: C++ uses `while CAS(lock, 0, tid)` spin-loop; TLA+ model checks `lock = 0` then assigns `lock := self`. The model assumes the lock is always immediately available when checked. Misses: concurrent contention exactly when both threads check at the same time (TLA+ sequentializes threads, so this can't happen in the model). **Risk: Low** — the property being checked (mutual exclusion of commit) is the same.
2. **Read-validate re-check (Low)**: C++ captures version, reads data, re-checks version. TLA+ atomicity makes re-check unnecessary within one step. **Risk: None** — PlusCal correctness is equivalent; the re-check is only needed in relaxed-memory C++.
3. **Memory ordering fences (Low)**: C++ has `atomic_signal_fence(seq_cst)` between set_lock_bits and inc_clock, and between write_back and update_ver. TLA+ model has no fences — single-copy atomicity means writes are immediately visible. **Risk: None** — fences prevent compiler reordering in C++ but TLA+ ordering is inherent.

## Cross-reference checklist template

```
Backend: <name>
Model file: <path>
C++ file: <path>

C++ atomic/action         | TLA+ action           | Match | Notes
--------------------------|-----------------------|-------|------
tx->active = true         | state[t] := "active"  | YES   |
snapshot = clock          | timestamp[t] := clock | YES   |
read_set record           | readSet[t] ∪ {<<a,v>>}| YES   |
write buffered            | writeBuf[t,a] := v    | YES   |
lock CAS(0, tid)          | lock' = t             | PART  | Spin not modeled
validate read-set         | ∀<<a,v>> ∈ readSet... | YES   |
write-back mem            | mem' = [a ∈ Addr ...] | YES   |
abort cleanup             | readSet[t] := {} ...  | YES   |
```

## Output format

Each audit produces:
1. **`docs/audits/<backend>.md`** — audit report
2. **`docs/audits/SUMMARY.md`** — summary table of all audited backends
3. **Model improvements** as PRs against the TLA+ files (if gaps are found)
