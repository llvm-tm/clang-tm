# Audit: PersistentSGL

**Score: 3/5** — Lock protocol well-captured; flush/recovery semantics differ from C++ implementation.

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/PersistentSGL.tla` (PlusCal, 263 lines) |
| C++ runtime | `backends/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp` (514 lines) |
| TLC config | `docs/proofs/persistent_sgl.cfg` |

## Algorithm Summary

SGL with NVM durability: lock, write to memory, write to persistent mmap file. On recovery, reload persisted symbol data from file. Bump allocator from persistent heap.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| begin(): global_tx_lock.lock() | L_idle: lock=self, state=active | ✅ | |
| end(): global_tx_lock.unlock() | L_complete: lock=0, state=idle | ✅ | |
| write_i4: `*addr=val; persist_write(off,...)` | L_active: Write: mem[a]=v, durable_log ∪ <<a,v>> | ⚠️ Partial | C++ writes to mem+pmem simultaneously; TLA+ has deferred flush |
| tm_init(): mmap, load persisted data | System: Recover: mem := nvm | ⚠️ Partial | TLA+ crash+recover vs C++ init load |
| Read: direct memory access | L_active: Read: skip | ✅ | |
| tm_exit(): msync + munmap | (no equivalent) | ❌ | TLA+ model doesn't exit |

## Invariants

| Invariant | C++ check | TLC result |
|-----------|-----------|------------|
| LockExclusion | std::mutex guarantees | ✅ PASS |
| LockHolderActive | lock → in TX | ✅ PASS |
| RecoveryConsistency | init restores symbols to mem | ✅ PASS |
| NVMContainsCommitted | dual-write ensures | ✅ PASS |

## Deviations

### 1. Flush model: deferred vs simultaneous (Medium risk)
**TLA+**: Write action buffers in `durable_log[t]`. A separate `Flush` action updates `nvm`. This imposes a phase ordering: write → (later) flush → complete.
**C++** (`PersistentSGL_runtime.cpp:96–136`): Each write hook writes to memory AND persists simultaneously. There is no deferred log — `persist_write` is called in the same function call as `*addr = val`.

The TLA+ model's deferred flush phase means that `nvm` can lag behind `mem` (dirty pages not yet flushed). In C++, `mem` and persistent storage are always in sync (dual write). The model captures a weaker durability guarantee than C++ provides.

**Risk**: Medium — the TLA+ model may find false-positive invariant violations that can't occur in C++ (e.g., crash between write and flush when C++ would have already persisted).

### 2. Crash/recovery as System process (Low risk)
**TLA+**: `System = 0` process with explicit crash (`crashed := TRUE`) and recovery (`mem := nvm; lock := 0`) actions. Crash can occur at any TLC step.
**C++**: Crash is real OS-level process termination. Recovery happens in `tm_init()` on next process start: it opens the persistent file, mmap's it, and copies persisted data back to symbol addresses.

The TLA+ model captures the effect of crash at any point during execution. C++ recovery happens only at process init, not during thread execution.

**Risk**: Low — the model captures worst-case crash behavior (any interleaving). C++ recovery is equivalent to the model's recovery action.

### 3. Durable log growth (No risk for bounded model)
**TLA+**: `durable_log[t]` grows unboundedly as `{(a,v), (a2,v2), ...}`. Bounded by ModelBound and MaxCommits for TLC.
**C++**: No log — direct writes to persistent mmap. No growth concern.

**Risk**: None for bounded model checking.

### 4. No memory-mapped I/O in TLA+ (No risk)
**C++** (`PersistentSGL_runtime.cpp:37–41`): 64 MB persistent mmap file, bump allocator, `RelPtr` offset_ptr for inter-restart pointer validity.
**TLA+**: `nvm` is a simple TLA+ function `[Addr → Data]`.

**Risk**: None — the storage mechanism is transparent to the TM protocol.

### 5. Read as `skip` (No risk)
**TLA+** (L_active Read branch): `skip` — read doesn't change any state.
**C++** (`real_tm_read_i4` etc.): Direct memory read `return *addr`.

**Risk**: None — reads are side-effect-free under the SGL.

## Summary

| Aspect | Verdict |
|--------|---------|
| Lock protocol (SGL) | ✅ Good match |
| Durability invariants | ✅ Pass TLC |
| Flush model | ⚠️ Deferred vs simultaneous |
| Crash/recovery | ✅ Captured as System process |
| mmap/bump allocator detail | ❌ Not modeled |
| **Overall score** | **3/5** |
