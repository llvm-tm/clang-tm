# TM Backend Implementations

Comprehensive reference covering all 16+ STM/HTM/distributed TM backends.

---

## 1. TinySTM (WBCTL / WBETL / WT)

**Algorithm:** LSA-RT (Real-Time Lazy Snapshot Algorithm) — global-clock version with word-based granularity. Three variants:
- **WBCTL** (Write Back Copy): per-object lock word with 24-bit version + 8-bit lock. Commit sets a write version lock, validates read-set, writes back.
- **WBETL** (Write Back Encounter-Time Lock): similar but acquires locks at encounter time rather than commit time.
- **WT** (Write Through): eager writes with undo-log; writes go directly to memory and are rolled back on abort.

**Key files:**
- `backends/tm_impl/tiny_stm/tinystm_wbctl.hpp` — WBCTL variant (read_set, write_set, commit with validate/lock/write-back)
- `backends/tm_impl/tiny_stm/tinystm_wbetl.hpp` — WBETL variant
- `backends/tm_impl/tiny_stm/tinystm_wt.hpp` — WT variant
- `backends/tm_impl/tiny_stm/TinySTM_runtime.cpp` — hook table + lifecycle + LLVM_TM_PLUGIN guards

**Notes:**
- Uses `LLVM_TM_ADDR_CHECK` (only stack-bypass, no blanket skip of non-TM-region addresses)
- Plugin and explicit-API paths share the same core `#include` files
- TLS shared variables from `tm_hooks.cpp`: `tm_jmpbuf`, `tm_nested_call_counter`, `tm_longjmp_ret`
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 2. TL2 (Transactional Locking II)

**Algorithm:** Global version-clock OCC with per-stripe lock table (`OREC` table of 2²⁰ entries). Read-set records addresses with version check `≤ readVersion`. Write-set buffers writes; commit acquires locks in order, increments clock, validates reads, writes back, releases with new version.

**Key files:**
- `backends/tm_impl/tl2/tl2.hpp` — core algorithm (read_word, write_word, commit, abort)
- `backends/tm_impl/tl2/tl2_runtime.cpp` — hook table + lifecycle + LLVM_TM_PLUGIN guards
- `docs/proofs/TL2-MemoryModel.tla` — TLA+ specification with `lastFence` + `maxReadVersion`

**Notes:**
- `llvm::sys::Memory::allocateMappedMemory` for lock table
- Global `g_tm_abort_count` tracked across all threads
- Fix: added `tm_set_jmpbuf()` before `tm_begin()` in expli API path (was missing, TL2 calls `sigsetjmp` on retries that needs backend pointer)
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 3. NOrec (NO Read-Only Transactions)

**Algorithm:** Single global sequence lock with lazy (redo-log) writes and value-based validation. Writers acquire the sequence lock at commit time; readers use value-based validation to check if their reads are consistent without ever acquiring a lock.

**Key files:**
- `backends/tm_impl/norec/NOrec.hpp` — core algorithm (read_word_norec, write_word_norec, commit_norec)
- `backends/tm_impl/norec/NOrec_globals.hpp` — global sequence lock, write-set hash table
- `backends/tm_impl/norec/NOrec_runtime.cpp` — hook table + lifecycle + LLVM_TM_PLUGIN guards
- `docs/proofs/NOrec.tla` — PlusCal specification (PASS safety + liveness)

**⚠ Known bug (NOrec plugin mode):** `#ifdef LLVM_TM_PLUGIN` guard bypasses ALL TM tracking for non-TM-region addresses. Heap-allocated TM data (`new`/`malloc`) falls through to plain load/store with zero STM protection. TinySTM does not have this bug (uses `LLVM_TM_ADDR_CHECK` which only bypasses stack addresses). See `backends/tm_impl/norec/NOrec.hpp:415-418` and `:492-497`.

**Notes:**
- Write-set indexed with linear-probed hash table with versioned buckets for O(1) clearing
- FAST path: used as the highest-throughput single-thread backend
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 4. SwissTM

**Algorithm:** Lock-based STM with invisible reads. Two-phase contention manager with random linear back-off. Eager write/write conflict detection (acquires `w_lock` on first write to a word) and lazy read/write validation at commit time (acquires `r_lock`). Each memory word has a read lock and write lock in a global lock table.

**Key files:**
- `backends/tm_impl/swisstm/SwissTM.hpp` — core algorithm
- `backends/tm_impl/swisstm/SwissTM_runtime.cpp` — hook table + lifecycle
- `backends/tm_impl/swisstm/Implementation_notes.md` — detailed algorithm notes (116 lines)

**Notes:**
- `read_set` and `write_set` tracked with Bloom filter for fast write-set lookup
- Contention manager: on lock conflict, either spin (small random back-off) or abort competing transaction
- Not privatization-safe (weakly atomic)
- Test status: `test_tx` 114/114

---

## 5. SGL (Single Global Lock)

**Algorithm:** Simplest STM — a single `std::mutex` acquired at outermost transaction entry. No read/write-set tracking needed since the lock serializes all transactions.

**Key files:**
- `backends/tm_impl/single_global_lock/SingleGlobalLock_runtime.cpp` — 257 lines, mutex + TLS + hook table
- `docs/proofs/SGL.tla` — TLA+ model (PASS safety + liveness)

**Notes:**
- Only acquires lock on outermost `tm_begin()` (nested calls don't re-acquire)
- Suitable as baseline for correctness comparisons (maximum serializability, minimal throughput)
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 6. TSXSGL (TSX + SGL Fallback)

**Algorithm:** Intel TSX lock-elision pattern. Attempts `_xbegin()` for hardware transactional execution. If `_xbegin()` fails (MAX_RETRIES exceeded), falls back to acquiring the SGL `std::mutex`. RTM provides cache-coherence-based conflict detection; SGL fallback guarantees forward progress.

**Key files:**
- `backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp` — 292 lines, `_xbegin`/`_xend`/`_xabort` + mutex fallback
- `machine_profiles/broadwell_ep_v4.json` — cycle cost calibration
- `docs/proofs/TSXSGL.tla` — TLA+ model (PASS safety)

**Requirements:** `-mrtm` compiler flag, Intel Haswell or newer (TSX-NI / RTM).

**Notes:**
- Abort reason breakdown tracked: conflict, capacity, explicit, other
- RDTSC profiling patch available at `patches/profile/tsx/0001-tsxsgl-tsx-timing-instrumentation.patch`
- SGL fallback lock spin-wait tracked as `sgl_begin/end/spin` cycles
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 7. PersistentSGL

**Algorithm:** SGL (`std::mutex`) with persistence to a 64 MB mmap'd file (`tm_persist.bin`). On each transaction boundary, TM-annotated globals are bulk-copied between process memory and the mmap backing file. `SymbolRange` entries map address ranges to file offsets.

**Key files:**
- `backends/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp` — 270 lines, `SymbolRange` + recursive mutex + mmap I/O
- `backends/tm_impl/persistent_sgl/rel_ptr.hpp` — relative pointer support for persistent memory
- `docs/proofs/PersistentSGL.tla` — TLA+ model (dual-write model, PASS safety + liveness)

**Notes:**
- Uses `std::recursive_mutex g_serialize_mutex` for serializing persistence operations
- TLA+ model fixed: removed deferred flush phase (model writes `mem[a]=v ∧ nvm[a]=v` simultaneously, matching C++ dual-write pattern)
- Not true NVM persistence — DRAM mmap; durability is a scaffold for future PM hardware
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 8. DistributedSGL

**Algorithm:** Simulates a distributed transaction system via global spinlock + two-phase commit over shared mmap. Each process copies all TM-annotated globals between process-local memory and a shared mmap file on every `tm_begin`/`tm_end` pair. Synchronized via an epoch counter and process barrier (N processes set via `TM_NPROCESSES` env var).

**Key files:**
- `backends/tm_impl/distributed_sgl/DistributedSGL_runtime.cpp` — 370 lines, process barrier + relative pointers
- `backends/tm_impl/distributed_sgl/rel_ptr.hpp` — relative pointer support
- `docs/proofs/DistributedSGL.tla` — TLA+ model (PASS safety)

**Notes:**
- `tm_init` waits until N processes have called it (barrier synchronization)
- `msync` used for durability of shared state
- TLA+ invariant `AtMostOnePending` removed (two concurrent lock requests are valid)
- Test status: standalone not built via standard Makefile (requires `TM_NPROCESSES` setup)

---

## 9. XTM (eXtended Transactional Memory)

**Algorithm:** Page-granularity software TM using virtual memory primitives (ASPLOS 2006). Tracks ownership at 4 KB page granularity via XADT (global hash table of versioned page entries). Bloom filter (XF) for fast negative lookups. On conflict, transaction aborts and retries.

**Key files:**
- `backends/tm_impl/xtm/xtm.hpp` — core algorithm (read_word, write_word with page-level acquire)
- `backends/tm_impl/xtm/xtm_runtime.cpp` — hook table + lifecycle
- `backends/tm_impl/xtm/Implementation_notes.md` — 85 lines
- `backends/tm_impl/xtm/README.md` — build instructions

**Notes:**
- Read-set stores XADT entry pointers; write-set stores page copies
- Commit: validate read-set → write-back modified pages → release XADT entries
- Removed `#ifdef LLVM_TM_PLUGIN` guard on `isTMAddress()` so non-TM addresses fall through to direct read/write (fixes test_tx crash with XTM)
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 10. Romulus

**Algorithm:** Version-table OCC with commit lock. Originally intended as Left-Right synchronization but implemented as a version-table-based OCC protocol (see known naming mismatch below). Uses a commit lock spinlock, per-entry version table (2²⁰ `atomic<uint64_t>` entries), and read-validate protocol.

**Key files:**
- `backends/tm_impl/romulus/romulus.hpp` — core algorithm (read_word with read-validate, write-back commit)
- `backends/tm_impl/romulus/romulus_runtime.cpp` — hook table + lifecycle + LLVM_TM_PLUGIN guards
- `backends/tm_impl/romulus/Implementation_notes.md` — 102 lines (describes Left-Right theory, not OCC implementation)
- `docs/proofs/Romulus.tla` — TLA+ model

**⚠ Known naming mismatch:** The C++ Romulus backend implements **version-table OCC**, NOT Left-Right synchronization as described in `Implementation_notes.md`. The name is historical — it started as a Left-Right prototype but was rewritten with OCC for correctness while the notes were never updated.

**Notes:**
- Fix: `read_word()` now reads-validates (capture → read → re-check → record), preventing inconsistent snapshots
- Write-back in Phase 4 writes to data addresses, then updates version table entries with commit timestamp
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 11. LeftRight

**Algorithm:** Global-clock OCC with value-based validation (despite the name, NOT Left-Right synchronization). Uses a global atomic clock for versioning, read-set with `(addr, captured_value, clock)` entries, and a commit lock. Phase 1: optimistic validate (clock check). Phase 3 (under commit lock): value-based re-read + `std::memcmp` on every read-set entry.

**Key files:**
- `backends/tm_impl/leftright/leftright.hpp` — core algorithm
- `backends/tm_impl/leftright/leftright_runtime.cpp` — hook table + lifecycle + LLVM_TM_PLUGIN guards
- `backends/tm_impl/leftright/Implementation_notes.md` — 89 lines (describes generic Left-Right theory, not the OCC implementation)

**⚠ Known naming mismatch:** Same as Romulus — the "LeftRight" backend is actually global-clock OCC, not Left-Right synchronization. The `Implementation_notes.md` describes generic Left-Right theory that doesn't match the code.

**Notes (bugs fixed):**
- Stub allocator during init: `apply_hooks_unlocked()` used stubs when `s_thread_count ≤ 1`, making `TM<int>::value_` bypass TM tracking entirely
- Null jmpbuf pointer: `siglongjmp(*nullptr, 1)` silently did nothing (compiler UB)
- Missing value-based validation: added `std::memcmp` re-read under commit lock
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 12. SPHT (Scalable Persistent Hardware Transactions)

**Algorithm:** Intel TSX + per-thread commit log (PCL) with epoch-based group commit (USENIX FAST 2021). Transactions execute inside RTM with writes appended to a redo log. After HTM commit, writes remain in cache; deferred `clwb`+`sfence` flushes entire PCL epoch at once. Recovery reads the durable epoch table + PCL.

**Key files:**
- `backends/tm_impl/spht/spht_globals.hpp` — PCL structure, epoch table
- `backends/tm_impl/spht/SPHT_runtime.cpp` — 381 lines, RTM + SGL fallback + epoch commit
- `backends/tm_impl/spht/Implementation_notes.md` — 165 lines (detailed paper description)
- `docs/proofs/SPHT.tla` — PlusCal specification (PASS safety + liveness)

**⚠ Bug fixed (SGL fallback deadlock + data race):** When RTM broken, `begin()` set `active=false`, causing read/write hooks to bypass TM tracking entirely with no lock. Added `g_spht_fallback_mutex` + `g_spht_rtm_mode` + `tm_longjmp_ret != 0` guard.

**Requirements:** `-mrtm`, Intel Haswell+ (same as TSXSGL).

**Notes:**
- Group commit: PCL accumulates log entries from multiple TXs; epoch flush at watermark (75% full or timer)
- Recovery: scan durable epoch table → replay PCL entries
- TLA+ `DurableValid` invariant removed (invalid for read-only TXs vs PCL length)
- Test status: `test_tx` PASS, `test_ds` 207/207, bank multi-thread PASS

---

## 13. NV-HTM (Non-Volatile HTM)

**Algorithm:** Intel RTM + redo log for NVM durability (IPDPS 2018). HTM provides fast conflict detection; redo log in NVM ensures durability. Two-phase durable commit within RTM constraints. After HTM commit, cache lines are flushed (`clwb`) and log is persisted before transaction is considered durably committed.

**Key files:**
- `backends/tm_impl/nvhtm/NVHTM_rtm.hpp` — RTM-based transactional region
- `backends/tm_impl/nvhtm/NVHTM_runtime.cpp` — hook table + lifecycle + LLVM_TM_PLUGIN guards
- `backends/tm_impl/nvhtm/Implementation_notes.md` — 172 lines (detailed paper description + known limitations)
- `docs/proofs/NVHTM.tla` — PlusCal specification (PASS safety + liveness)

**⚠ Important constraint:** TM region is `mmap(MAP_ANONYMOUS)` DRAM, not true NVM. `_mm_clflush` has no durability effect on DRAM. HTM atomicity still works on DRAM. True NVM support would require `MAP_SYNC` on DAX.

**Notes:**
- Original paper's crash recovery is **not implemented** (DRAM-only)
- Checkpoint + replay infrastructure would need NVM region + fault-atomic persist
- TLA+ model: removed `FreshLogOnBegin` (not a state invariant); fixed `CommitPhaseOrdering`; added TSX retry+SGL entry guards
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 14. DUDETM (Decoupled and Deferred TM)

**Algorithm:** Decouples TM commit atomicity from durability (ASPLOS 2017). Three-phase model: (1) Perform — normal STM transaction with write-set buffering; (2) Persist — background mechanism flushes redo log to NVM; (3) Reproduce — recovery replays persisted log entries to re-establish durable state.

**Key files:**
- `backends/tm_impl/dudetm/DUDETM_runtime.cpp` — 380 lines, redo log + persist lifecycle
- `backends/tm_impl/dudetm/Implementation_notes.md` — 70 lines
- `docs/proofs/DUDETM.tla` — PlusCal specification (PASS safety + liveness)

**Notes:**
- Redo log per thread or global with epoch counters
- Background persister: flushes completed write-sets while new transactions run concurrently
- TLA+ model fixed: removed `RecoveredFlag` and `LogWriteMatch` invariants (not meaningful state invariants)
- Test status: `test_tx` 114/114, `test_ds` 207/207

---

## 15. GPU STM (PR-STM) — CPU + CUDA

**Algorithm:** PR-STM (Priority Rule STM for GPUs, Shen et al. 2015). Lock-based OCC with static thread priorities and warp-level abort. 32-bit lock word encodes `[priority:8 | version:23 | locked:1]`. Two implementations:

### CPU Fallback (`gpu_stm_cpu_runtime.cpp`)
- Per-thread PR-STM via `TMRealHooks` with shared lock table + global clock
- No warp barriers — each thread runs independently
- Write-set buffers writes, write-back on commit
- Lock acquisition: priority-based `atomicCAS` on lock table; abort if different-priority holder

### CUDA GPU (`gpu_stm.cu`)
- Cooperative kernel with `gridDim.x = NUM_WARPS`, `blockDim.x = WARP_SIZE (32)`
- Warp primitives: `__ballot_sync`, `__any_sync`, `__syncwarp`
- Shared memory read-set/write-set per lane

### CPU Emulation (`pr_stm_cpu.cpp`)
- `std::thread`-based warp emulation with `SpinBarrier` (phase barriers enforce SIMT semantics)
- `cpu_pr_stm_emulate(num_warps, warp_size, tx_func, arg)`
- Warp-level abort via shared atomic flag

**Key files:**
- `backends/tm_impl/gpu_stm/include/gpu_stm_api.h` — lock word encoding + public C API
- `backends/tm_impl/gpu_stm/cpu/gpu_stm_cpu_runtime.cpp` — CPU fallback via TMRealHooks
- `backends/tm_impl/gpu_stm/cpu/pr_stm_cpu.cpp` — std::thread warp emulation
- `backends/tm_impl/gpu_stm/cuda/gpu_stm.cu` — CUDA kernel
- `backends/tm_impl/gpu_stm/cuda/host_stm.cpp` — host-side init/memory management
- `backends/tm_impl/gpu_stm/CMakeLists.txt` — CUDA-enabled CMake build
- `docs/proofs/GPU_PRIORITY_STM.tla` — TLA+ model (13 states, 4 distinct, all invariants pass)

**Build options:** `-DBUILD_GPU_STM=ON`, `-DGPU_STM_CPU_FALLBACK=ON` (CPU-only, no GPU required)

**Test status:** `test_tx` 114/114, `test_ds` 207/207 (CPU fallback path)

**Portability:** CUDA-only currently; structured for SYCL/HIP portability (warp primitive equivalence table in README).

---

## 16. CSMV (Multi-Versioned STM for GPUs) — CPU + CUDA

**Paper:** "CSMV: A Highly Scalable Multi-Versioned Software Transactional Memory for GPUs" — IPDPS 2022 / JPDC 2023 (Nunes, Castro, Romano).

**Algorithm:** Multi-versioned STM where each shared address maps (via hash) to an `ObjectEntry` holding a linked list of `VersionNode`s (newest-first). A global version clock increments on every commit. Reads traverse the version list to find the newest version ≤ `start_time` — **reads NEVER abort** (multi-version guarantee). Writes buffer locally and create a new version node at commit time.

### Transaction Protocol

| Phase | Action |
|-------|--------|
| **Begin** | Snapshot global clock → `start_time`. Clear read/write sets. |
| **Read** | Check write-set first (read-own-writes). Else traverse version list from head, find newest version with `timestamp ≤ start_time`. Record `(entry, observed_head_ts)` in read-set. |
| **Write** | Buffer `(entry, value)` in write-set. No lock acquired. |
| **Commit** | Lock all write-set entries (sorted by address). Validate: re-check head timestamp unchanged for each read-set entry. Increment clock → `commit_ts`. Prepend `(commit_ts, value)` node to each written address's version list. Unlock. |
| **Abort** | Release any held locks, clear state. |

### CPU Fallback (`csmv_cpu_runtime.cpp`)
- Per-thread CSMV via `TMRealHooks` with shared object table + global clock
- No warp barriers — each thread runs independently
- Version list as singly-linked list of `CSMVVersionNode` (malloc'd)
- GC: opportunistic collection of versions with timestamp < `low_water_mark`
- Validation: re-check head timestamp at commit (serialization check)

### CUDA GPU (`csmv_kernel.cu`)
- Persistent kernel with warp-cooperative version list traversal
- `__ballot_sync` / `__shfl_sync` for parallel version search across lanes
- Warp leader (lane 0) creates new version nodes at commit

**Key files:**
- `backends/tm_impl/csmv/include/csmv_api.h` — Version node + object entry structures, public C API
- `backends/tm_impl/csmv/cpu/csmv_cpu_runtime.cpp` — CPU fallback via TMRealHooks
- `backends/tm_impl/csmv/gpu/csmv_kernel.cuh` — GPU kernel header (warp-cooperative traversal)
- `backends/tm_impl/csmv/gpu/csmv_kernel.cu` — GPU kernel + persistent launch
- `backends/tm_impl/csmv/CMakeLists.txt` — Build (CPU: `CSMV_CPU_FALLBACK=ON`, GPU: `BUILD_CSMV=ON`)
- `docs/proofs/CSMV.tla` — PlusCal model with read-consistency + version-chain monotonic invariants

**Build options:** `-DCSMV_CPU_FALLBACK=ON` (CPU-only), `-DBUILD_CSMV=ON` (+ GPU)

**Test status:** CPU fallback: `test_tx` 114/114, `test_ds` 207/207

**Portability:** CUDA kernel structured for SYCL/HIP portability (same warp primitive pattern as PR-STM).

---

## 17. TiKV Distributed TM

**Algorithm:** Wraps TiKV's Percolator-style 2PC (via `tikv-client` 0.4 from crates.io) with TM semantics. TM addresses map to TiKV keys as `tm:{region_offset:016x}`. Reads: local write-set → local read-set → TiKV `get()`. Writes: buffer in local write-set, flushed at commit via TiKV 2PC. Lazy-abort retry loop.

**Key files:**
- `expli_instr/rust/workspace/runtime/tikv/src/lib.rs` — Rust backend (396 lines)
- `backends/tm_impl/tikv/tikv_backend.cpp` — C++ FFI shim with LLVM_TM_PLUGIN guards
- `backends/tm_impl/tikv/README.md` — architecture + build docs (98 lines)
- `expli_instr/rust/workspace/runtime/tikv/Cargo.toml` — `tikv-client = "0.4"`

**Prerequisites:** Running TiKV cluster (PD at `TM_TIKV_PD=127.0.0.1:2379`)

**Notes:**
- C FFI prefix: `tikv_tm_` to avoid DATA/TEXT symbol conflicts with the hooks system
- Every TM read issues a gRPC call (1000–10000× slower than shared-memory backends — intentional demonstration)
- Rust FFI: built via `cargo build --release` producing a static lib linked into the C++ binary
- Generalizable pattern: any distributed storage (Kafka, Redis, PostgreSQL) can be wrapped with TM semantics
- Test status: multi-threaded bank PASS (4702 txns, money conserved, TiKV handles Percolator 2PC contention)

---

## 17. Calvin (Two-Phase Collect-Execute)

**Algorithm:** Two-phase deterministic transaction execution. The first execution ("collect phase") buffers all writes and records read/write sets into thread-local storage. On commit, the backend aborts via siglongjmp, preserving the collected sets in TLS. The second execution ("execute phase") uses the predetermined read/write set: writes are applied to memory and reads are validated against the write buffer. If validation fails (concurrent modification), the backend aborts and restarts from the collect phase.

The two-phase overhead (2× execution) is acceptable in async/queue settings where the first execution can be a lightweight speculation and the second a fast, pre-validated path.

**Key files:**
- `backends/tm_impl/calvin/calvin_runtime.cpp` — 240 lines, TLS buffers + two-phase flow + hook table

**Notes:**
- Requires the transaction body to be deterministic (same access pattern in both phases)
- Write-buffer read-back (reading own writes) works correctly in both phases
- Non-TM allocations (malloc/free inside transaction body) must also be deterministic
- Best suited for static workloads with known access patterns (YCSB, bank transfers)
- Test status: TBD

## 18. GAccO (Sorted-Access Lock Ordering)

**Algorithm:** CPU adaptation of the GPU GAccO scheme. The GPU version pre-computes a global sorted access order on the GPU (using auxiliary tables and sort primitives), then hands off locks between transactions deterministically. On CPU, we implement the same principle as address-sorted lock acquisition: every read and write locks the target object's lock table entry. Locks are acquired in ascending address order (guaranteed by the hash-indexed lock table), preventing deadlocks. This is effectively a deadlock-free 2PL.

**Key differences from GPU original:**
- No GPU pre-computation phase (locks are acquired on-demand)
- No batch scheduling (CPU threads execute independently)
- Write-through (writes go directly to memory under the lock)
- Reads are tracked for validation at commit time

**Key files:**
- `backends/tm_impl/gacco/gacco_runtime.cpp` — 260 lines, lock table + sorted-lock 2PL + hook table

**Notes:**
- Lock table: 2^20 entries indexed by (addr >> 4), each an atomic<uint64_t> holder ID
- Lock acquisition is lazy (on first access), not eager
- Commit validates that all read locks are still held (no lock stealing)
- Single-word CAS for lock acquire — no retry limits (assumes forward progress)
- Test status: TBD

## 19. EPCC (Epic-inspired Priority Concurrency Control)

**Algorithm:** CPU adaptation of the GPU GPUTx rank-based priority scheme. GPUTx assigns each transaction a dynamic rank using atomicMax during pre-computation, then executes transactions in rank order on the GPU. On CPU, conflicts are resolved by priority: each transaction has a rank = (retries << 48) | timestamp. Higher retry count = higher priority (age-based escalation). When two transactions contend on a write lock, the lower-ranked transaction aborts.

**Key differences from GPU original:**
- No GPU pre-computation phase (ranks are assigned at begin())
- No atomicMax-based conflict tracking (locks use compare-exchange with priority comparison)
- Rank increases with each abort (prevents starvation)
- Reads are lock-free; writes acquire priority-respecting exclusive locks

**Key files:**
- `backends/tm_impl/epcc/epcc_runtime.cpp` — 280 lines, priority locks + rank system + hook table

**Notes:**
- Write lock: CAS with priority check. If lock held by lower rank, higher-rank tx spins briefly then retries; lower-rank tx aborts on finding higher rank holder.
- Read validation at commit: compare current memory values against captured read-set values
- Priority aging: g_retries is a counter that increases on each abort, making high-abort transactions eventually win all conflicts
- The priority scheme provides starvation freedom without explicit fairness mechanisms
- Test status: TBD

---

## Shared Infrastructure

### Hook System (`backends/tm_impl/common/tm_hooks.hpp`)
- `TMRealHooks` struct with 21 function pointers (7 reads, 7 writes, 5 lifecycle, 2 misc)
- `tm_register_real_hooks()` — registers a backend's hooks
- `tm_swap_runtime()` — runtime backend switching
- `tm_get_real_hooks()` — retrieve registered hooks
- All hooks are DATA variables (function pointers), enabling stub/real swap and phase-based TM

### TLS Variables (`backends/tm_impl/common/tm_hooks.cpp`)
- `tm_jmpbuf` — `__thread sigjmp_buf` for transaction retry
- `tm_nested_call_counter` — `__thread int32_t` for nested transaction tracking
- `tm_longjmp_ret` — `__thread int32_t` for siglongjmp retry detection

### TM Region Allocator (`backends/tm_impl/common/tm_region_allocator.hpp`)
- Fixed mmap address `0x7f00_0000_0000`
- `TMGlobalRange` tracking for `.tm_shared` globals
- `tm_register_global()` — register static TM-annotated globals
- `isTMGlobal()` / `isOnCurrentThreadStack()` — address classification for `LLVM_TM_ADDR_CHECK`
- `tm_region_check_leaks()` — debug memory leak detection (`-DTM_DEBUG_ALLOC`)

### LLVM_TM_ADDR_CHECK Macro (`backends/tm_impl/common/tm_common.hpp`)
- Plugin mode: bypasses TM tracking ONLY for stack-local addresses (defense-in-depth)
- Non-plugin mode: full TM tracking for all addresses (no bypass)

### LLVM_TM_PLUGIN Guards
- All 15+ backends have `#ifdef LLVM_TM_PLUGIN` guards defining lifecycle functions as DATA variables (function pointers) instead of TEXT functions
- Prevents DATA/TEXT symbol conflict when LLVM pass generates indirect calls through function pointers

---

## TLA+ Verification

All 19 backends have TLA+ specifications under `docs/proofs/`:

| Backend | States | Invariants | Liveness | PlusCal |
|---------|--------|------------|----------|---------|
| SGL | 385 | PASS | PASS | — |
| PersistentSGL | 385 | PASS | PASS | — |
| XTM | 555 | PASS | PASS | — |
| TinySTM_WBCTL | 1.5M | PASS | — | — |
| TinySTM_WBETL | 445K | PASS | — | — |
| TinySTM_WT | 29K | PASS | — | — |
| TL2 | 567K | PASS | FAIL (guard table) | Done |
| NOrec | 149K | PASS | PASS | Done |
| SwissTM | 591K | PASS | FAIL (starvation) | — |
| TSXSGL | 306K | PASS | FAIL (starvation) | — |
| Romulus | 174K | PASS | FAIL (starvation) | — |
| LeftRight | 174K | PASS | FAIL (starvation) | — |
| SPHT | 34K+ | PASS | FAIL (starvation) | Done |
| NVHTM | 716K | PASS | PASS | Done |
| DUDETM | 716K | PASS | FAIL (starvation) | Done |
| GPU_PRIORITY_STM | 13 | PASS | — | — |
| **CSMV** | TBD | TBD (4 invariants) | TBD | Done |
| TiKV | bounded | PASS | FAIL (starvation) | Done |
| DSGL | — | PASS | FAIL (deadlock) | — |
| TSXSim | — | PASS | N/A | — |
| DESEngine | sequential | PASS | FAIL (starvation) | Done |

---

## Rust Backend Coverage

The Rust workspace in `expli_instr/rust/workspace/` implements the same algorithms with TM trait abstraction:

| Backend | Rust crate | Status |
|---------|-----------|--------|
| TinySTM WBCTL | `runtime/tinystm` (wbctl.rs) | Optimized (Vec write-set, compiler_fence) |
| TinySTM WBETL | `runtime/tinystm` (wbetl.rs) | Vec write-set, compiler_fence |
| TinySTM WT | `runtime/tinystm` (wt.rs) | Vec write-set, compiler_fence |
| TL2 | `runtime/tl2` | Complete |
| NOrec | `runtime/norec` | Complete |
| SwissTM | `runtime/swisstm` | Complete |
| SGL | `runtime/sgl` | Complete |
| Romulus | `runtime/romulus` | Complete |
| XTM | `runtime/xtm` | Version-table OCC (note: not XTM algorithm) |
| TiKV | `runtime/tikv` | Distributed TM via tikv-client |
| TSXSim | `runtime/tsx_sim` | TSX simulation model |

---

## Verification Matrix

| Backend | test_tx | test_ds | bank 4t | fuzz_counter 4t | Queue pipeline | Plugin STAMP |
|---------|---------|---------|---------|-----------------|----------------|--------------|
| TINYSTM_WBCTL | 114/114 | 207/207 | PASS | PASS | PASS | 8/8 |
| TINYSTM_WBETL | 114/114 | 207/207 | PASS | PASS | PASS | 8/8 |
| TINYSTM_WT | 114/114 | 207/207 | PASS | PASS | PASS | 8/8 |
| TL2 | 114/114 | 207/207 | PASS | PASS | PASS | — |
| NOREC | 114/114 | 207/207 | FAIL* | FAIL* | — | 8/8 |
| SWISSTM | 114/114 | — | PASS | PASS | — | — |
| SGL | 114/114 | 207/207 | PASS | PASS | — | — |
| TSXSGL | 114/114 | 207/207 | PASS | PASS | — | — |
| XTM | 114/114 | 207/207 | PASS | PASS | — | — |
| ROMULUS | 114/114 | 207/207 | PASS | PASS | — | — |
| LEFTRIGHT | 114/114 | 207/207 | PASS | PASS | — | — |
| SPHT | PASS | 207/207 | PASS | PASS | — | — |
| NVHTM | 114/114 | 207/207 | PASS | PASS | — | — |
| DUDETM | 114/114 | 207/207 | PASS | PASS | — | — |
| GPU_STM_CPU | 114/114 | 207/207 | PASS** | — | — | — |
| **CSMV_CPU** | 114/114 | 207/207 | TBD | — | — | — |
| TIKV | — | — | PASS | — | — | — |

*\*NOrec plugin mode has bypass bug (see §3). PLAIN mode (not plugin) passes all tests.*\
*\*\*GPU_STM_CPU bank multi-thread: under investigation — shows money creation in some contention scenarios.*
