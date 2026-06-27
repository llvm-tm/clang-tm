# DistributedSGL Audit Report

## Score: **1/5**

The TLA+ model and C++ implementation describe fundamentally different algorithms. The model specifies a client-server network message-passing protocol with a lock server; the implementation is a single-machine file-backed mmap spinlock. Five invariants verified on the model have no counterpart in C++ because the C++ code provides no distributed semantics, read-set tracking, write-set tracking, or conflict detection.

---

## Files

| Artifact | Path | Status |
|----------|------|--------|
| TLA+ spec | `docs/proofs/DistributedSGL.tla` | ✅ 179 lines, 5 invariants |
| TLC config | `docs/proofs/DistributedSGL.cfg` | ✅ 2 clients, 2 addresses, 3 data values |
| C++ header | `backends/tm_impl/distributed_sgl/DistributedSGL.hpp` | ❌ **Missing** — no header file exists |
| C++ runtime | `backends/tm_impl/distributed_sgl/DistributedSGL_runtime.cpp` | ✅ 375 lines |
| Implementation notes | `backends/tm_impl/distributed_sgl/Implementation_notes.md` | ❌ **Missing** |

---

## Algorithm Summary (TLA+)

A client-server SGL protocol: N client nodes send `LOCK_REQ` messages to a lock server, receive `LOCK_GRANT`, perform reads/writes on shared memory, then send `UNLOCK`. The server grants to at most one client at a time. This is a straightforward extension of `SGL.tla` with explicit message-passing.

## Algorithm Summary (C++ Implementation)

A single-machine file-backed `mmap` SGL: multiple OS processes share a file on disk (`benchmark_results/tm_2pc_state.bin`). `tm_init` synchronises via a barrier (waiting for `TM_NPROCESSES` processes). `tm_begin` acquires a shared-memory spinlock and copies all TM-annotated globals FROM the mmap into process-local memory. `tm_end` copies all TM symbols back TO the mmap, increments an epoch counter, and releases the spinlock. Read/write hooks are plain memory loads/stores — no read-set, no write-set, no validation, no undo logging.

---

## Cross-Reference Checklist

| C++ Function / Pattern | TLA+ Label / Action | Match |
|------------------------|---------------------|-------|
| `real_tm_begin()` — spinlock + `sync_shared_to_local()` | `SendLockReq` + `ProcessLockReq` + `RecvLockGrant` | ❌ TLA+: 3-way message exchange. C++: single CAS spinlock in shared memory. |
| `real_tm_end()` — `sync_local_to_shared()` + epoch++ + unlock | `SendUnlock` + `ProcessUnlock` | ❌ TLA+: two messages (UNLOCK + server processes it). C++: direct shared-memory write + CAS unlock. |
| `real_tm_read_i4` / `real_tm_write_i4` etc. — plain dereference | `ClientRead` / `ClientWrite` | ⚠️ Surface match (read/write memory) but C++ has zero TM tracking (no read-set, no ownership check). |
| `tm_init` — barrier, `process_count`, `sync_local_to_shared()` | None | ❌ No TLA+ init equivalent. Model starts with all variables initialized; C++ runtime init is complex (mmap, barrier, symbol enumeration). |
| `tm_exit` — countdown + `munmap` + `unlink` | None | ❌ Not modeled. |
| `g_state->lock_flag` (shared spinlock) | `lock_holder` variable | ⚠️ Similar purpose (mutual exclusion) but different mechanism: TLA+ uses explicit grant, C++ uses CAS. |
| `g_state->epoch` | `version` | ⚠️ Both increment on commit. TLA+ version used only in spec scaffolding; C++ epoch is never read. |
| `g_state->ready_count` (barrier at init) | None | ❌ Not modeled. |
| Shared mmap file | `mem` array | ⚠️ Both store shared state. C++ stores a snapshot of all TM symbols; TLA+ `mem` is a `[Addr -> Nat]` function for individual addresses. |
| `g_first_begin` / `sync_local_to_shared()` on first `tm_begin` | None | ❌ Not modeled. |
| `LLVM_TM_PLUGIN` guards (DATA vs TEXT) | None | ❌ Not modeled. |
| `rel_ptr.hpp` / `RelPtr::set_base(base)` | None | ❌ Not modeled. |

---

## Invariants

Ran with:
```sh
java -cp /tmp/tla2tools.jar tlc2.TLC -deadlock docs/proofs/DistributedSGL.tla \
  -config docs/proofs/DistributedSGL.cfg
```

| Invariant | TLC Result | Verified on C++? | Notes |
|-----------|------------|------------------|-------|
| **LockExclusion** (I1) — at most one `lock_holder` | ✅ PASS | ❌ | C++ `lock_flag` spinlock achieves the same effect via CAS, but `lock_holder` identity (which client) is not tracked — only a binary locked/unlocked flag. |
| **LockHolderHasGrant** (I2) — holder believes it has lock | ✅ PASS | ❌ | C++ has no `granted` flag; the spinlock holder implicitly "has the grant" by having acquired the CAS. |
| **NoSpuriousGrant** (I3) — no client holds grant without being holder | ✅ PASS | ❌ | Not applicable — C++ has no grant concept. |
| **AtMostOnePending** (I4) — at most one pending LOCK_REQ and no concurrent grant-in-flight with a pending req | ✅ PASS | ❌ | C++ has no message queue; pending requests are implicit in CAS contention (hw-level arbitration). |
| **ServerConsistency** (I5) — at most one LOCK_GRANT in flight | ✅ PASS | ❌ | No server, no grants in flight. |

**TLC result**: 4 states generated, 4 distinct, depth 3, all invariants PASS, no deadlock.

---

## Deviations

1. **Model describes client-server network protocol; C++ is shared-memory mmap SGL**  
   The TLA+ spec models three explicit message types (`LOCK_REQ`, `LOCK_GRANT`, `UNLOCK`) exchanged between N clients and a central lock server over a message queue. The C++ implementation uses a shared-memory `mmap` file with a CAS spinlock. There is no network, no message queue, no lock server, and no client-server topology. The C++ implementation is a standard single-machine SGL with no distributed semantics whatsoever.

2. **No `ProcessLockReq` / `RecvLockGrant` / `ProcessUnlock` in C++**  
   These three actions form the core of the TLA+ spec (the server receives and processes lock requests, grants them one at a time, and processes unlocks). In C++, `real_tm_begin` atomically acquires the spinlock via `compare_exchange_weak` — there is no intermediary "waiting" state, no server decision, no grant message.

3. **C++ read/write hooks have zero TM tracking**  
   The TLA+ model assumes that `ClientRead` and `ClientWrite` occur under the protection of the lock, which is correct in both. However, the C++ hooks are plain memory dereferences (`return *a`, `*a = v`) with no read-set capturing, no write-set recording, no version checking, and no conflict detection. This means the C++ backend cannot detect when a transaction reads stale data or when a concurrent write conflicts — it relies entirely on the coarse-grained global spinlock for serialisation.

4. **C++ has a complex init/exit lifecycle not reflected in TLA+**  
   `tm_init` opens a file, `mmap`s it, sets up process-count barrier, handles the `TM_NPROCESSES` environment variable, and distinguishes "first process" (writes initial data) from "subsequent processes" (reads shared data). `tm_exit` decrements the barrier and optionally unmaps+unlinks. The TLA+ spec has `Init` only (all variables initialised to constants).

5. **No `g_first_begin` / first-transaction publish in TLA+**  
   The C++ code publishes local state to the mmap on the first `tm_begin` per thread (because benchmarks initialise TM globals after `tm_init`). This is a workaround for a timing issue that has no analogue in the TLA+ model.

6. **`epoch` in C++ is incremented but never read**  
   The TLA+ `version` variable serves as a logical clock. In C++, `g_state->epoch` is incremented at each commit (`fetch_add(1)`) but no code ever reads its value — it is dead state that does not contribute to correctness or conflict detection.

7. **No `pc` (program counter) state machine in C++**  
   The TLA+ models each client through states `"idle"` → `"waiting"` → `"active"` → `"done"` → `"idle"`. The C++ code has no such state machine: `real_tm_begin` and `real_tm_end` are the only states, and there is no concept of waiting for a grant or marking completion.

8. **Conflicting names: "2PC" in comments is not two-phase commit**  
   The C++ code labels phases "PREPARE" (acquire lock + sync) and "COMMIT" (sync + release), suggesting a two-phase commit protocol. True 2PC requires a coordinator, prepare votes, and a commit decision. The C++ implementation is a single-phase coarse-grained lock-and-copy — no voting phase, no prepare/commit distinction, no recovery protocol.

9. **Missing header file (`DistributedSGL.hpp`)**  
   No C++ header exists for this backend. The entire implementation lives in the single `.cpp` file.

10. **Missing `Implementation_notes.md`**  
    The implementation notes file that typically documents design decisions, protocol details, and abstraction choices does not exist for this backend.

---

## Summary

| Dimension | Finding |
|-----------|---------|
| **TLA+ quality** | Clean model, 5 well-stated invariants, all verified by TLC with no deadlock. Reasonable extension of `SGL.tla`. |
| **Model vs code mismatch** | **Critical** — the model describes a client-server network protocol with explicit message passing; the code is a single-machine `mmap` spinlock SGL. |
| **Implementation completeness** | Low — no header file, no read-set/write-set, no conflict detection, no abort, no rollback, no recovery. Relies entirely on coarse-grained lock-and-copy. |
| **What the C++ implementation actually is** | A shared-memory file-backed SGL with process synchronisation via barrier at init and CAS spinlock at transaction boundaries. "Distributed" only in the sense that multiple OS processes on the same machine share an `mmap` file. |
| **Risk** | **High** — the TLA+ model cannot validate the C++ implementation because they solve different problems at different abstraction levels. A bug in the C++ code (e.g. stale data on first transaction, race in the barrier, or the `epoch` being dead code) cannot be found by model-checking this spec. |
| **Recommendation** | Either rewrite the TLA+ spec to model the actual implementation (mmap, spinlock, symbol sync, epoch counter) or rewrite the C++ backend to implement the client-server message-passing protocol the TLA+ spec describes. These two artifacts should be unified under one algorithm. |
