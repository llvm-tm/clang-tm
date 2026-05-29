# NV-HTM — Implementation Notes

**Paper**: "NV-HTM: A Persistent Transactional Memory for Non-Volatile Memory with Hardware Transactional Memory" (IPDPS 2018, extended in JPDC 2019)

## Overview

NV-HTM is a **persistent HTM** that extends Intel RTM (Restricted Transactional Memory) with redo logging for NVM durability. The core insight: HTM provides fast, hardware-accelerated conflict detection and atomic commit, but HTM transactions are not durable (cache flushes before the TsxAbort boundary are discarded). NV-HTM adds a redo log in NVM and a two-phase durable commit protocol that works within the constraints of Intel RTM.

## Design

### Redo-Log-Only Approach
Unlike DUDETM (which uses a separate persister thread) or SPHT (which uses group commit), NV-HTM writes the redo log entries **within** an HTM transaction and only **persists** them after the HTM commit succeeds.

### Protocol (within a single HTM transaction)

1. **Begin**: Open an RTM transaction (`xbegin`). Allocate a per-transaction redo-log buffer in NVM (pre-allocated per thread, e.g. 4KB).

2. **Write**: Append `(addr, size, new_value)` to the in-log buffer. This write is inside the HTM and will be rolled back if the HTM aborts. Do NOT write to `*addr` yet (undo-in-place could leave partial state if crash occurs before redo log is durable).

3. **Read**: Read from `*addr` directly (same as normal STM — no indirection needed since HTM tracks the read-set).

4. **HTM Commit** (`xend`): Atomically makes the log buffer writes visible to other threads.

5. **Durable Phase** (AFTER `xend`, no longer in HTM):
   - **Step A (Flush Log)**: Clwb each cache line of the redo log buffer.
   - **Step B (Checkpoint)**: Write the log-buffer address to a durable checkpoint region (clwb + sfence).
   - **Step C (Apply)**: For each log entry, write `new_value` to `*addr` (undo-in-place: now the primary location is updated). Since Step A + B happened first, even if a crash occurs during Step C, the recovery can find the checkpointed log and replay remaining entries.
   - **Step D (Clear Checkpoint)**: Clear the checkpoint so recovery knows there is no pending log.

### Recovery
1. Scan the checkpoint region. If a valid checkpoint exists:
   - Find the redo log buffer referenced by the checkpoint
   - Replay all entries (addr <- value)
   - Clear the checkpoint
2. Resume normal operation.

## Key Differences from DUDETM and SPHT

| Property | NV-HTM | DUDETM | SPHT |
|----------|--------|--------|------|
| Conflict detection | HTM (hardware) | STM (software) | HTM (hardware) |
| Persist timing | after HTM commit, before write-back | async after STM commit | group commit after HTM commit |
| Log flush | inline with TX (Step A) | background thread | per-thread with epoch sync |
| Write visibility | HTM atomic | STM version-clock | HTM atomic + epoch |

## Implementation Sketch for clang-tm

Since NV-HTM requires RTM instructions (`xbegin`, `xend`, `xabort`), it cannot be implemented purely in C++ — it needs inline assembly or compiler intrinsics (`_xbegin()`, `_xend()`, etc.):

```cpp
// Pseudocode — key data structures
struct NvHtmLogEntry {
    void *addr;
    uint64_t size;
    char data[0]; // flexible array member for variable-size payload
};

struct NvHtmTx {
    char *log_buf;       // pre-allocated NVM buffer (e.g. per-thread slab)
    uint64_t log_offset; // next free byte in log_buf
    // checkpoint in NVM:
    char *checkpoint_ptr; // points to log_buf if pending, nullptr if clear
};

static inline bool nvhtm_begin(NvHtmTx *tx) {
    unsigned status = _xbegin();
    if (status == _XBEGIN_STARTED) {
        // In HTM — set up tx
        tx->log_offset = 0;
        // Clear checkpoint marker (in-NVM so crash doesn't see stale checkpoint)
        __atomic_store_n(tx->checkpoint_ptr, (char*)nullptr, __ATOMIC_RELAXED);
        return true;
    }
    // HTM abort — handle capacity/conflict retry
    return false;
}

static inline void nvhtm_write(NvHtmTx *tx, void *addr, uint64_t val, size_t size) {
    // Log entry inside HTM (will be rolled back if HTM aborts)
    NvHtmLogEntry *e = (NvHtmLogEntry*)(tx->log_buf + tx->log_offset);
    e->addr = addr;
    e->size = size;
    memcpy(e->data, &val, size);
    tx->log_offset += sizeof(NvHtmLogEntry) + size;
}

static inline bool nvhtm_commit(NvHtmTx *tx) {
    _xend(); // Step: HTM commit (atomic visibility of log buffer)

    // Durable phase (out of HTM):
    // Step A: flush log buffer to NVM
    for (size_t off = 0; off < tx->log_offset; off += 64)
        _mm_clwb(tx->log_buf + off);
    _mm_sfence();

    // Step B: write checkpoint
    *tx->checkpoint_ptr = (char*)(uintptr_t)tx->log_buf;
    _mm_clwb(tx->checkpoint_ptr);
    _mm_sfence();

    // Step C: apply writes
    for (size_t off = 0; off < tx->log_offset; ) {
        NvHtmLogEntry *e = (NvHtmLogEntry*)(tx->log_buf + off);
        memcpy(e->addr, e->data, e->size);
        off += sizeof(NvHtmLogEntry) + e->size;
    }

    // Step D: clear checkpoint
    *tx->checkpoint_ptr = (char*)nullptr;
    _mm_clwb(tx->checkpoint_ptr);
    _mm_sfence();

    return true;
}
```

## Limitations

1. **RTM capacity**: HTM L1/L2 cache capacity (~256KB L1, ~1MB L2) limits the size of the redo log that fits in a single HTM transaction. Large TXs must fall back to an STM software path or split into multiple HTM TXs.

2. **Checkpoint durability ordering**: Steps B (checkpoint) must be visible *before* Step C (apply) can survive a crash. The clwb + sfence ordering is critical — any weak ordering in the memory subsystem (e.g., stores reordered across cachelines) could leave a checkpoint pointing to an incomplete log.

3. **clwb granularity**: The clwb instruction flushes a single cacheline (64 bytes). Log entries smaller than 64 bytes are still flushed at the cacheline granularity, wasting NVM bandwidth.

4. **rtm_abort in recovery**: If the HTM transaction aborts after some log entries but before the HTM commit (`xend`), the log buffer contents are reverted by the hardware. This is correct — the TX never committed. If the HTM aborts during Step C (apply), the checkpoint is already set and recovery will replay the log fully.

## When to Use

- **Small-to-medium TXs** (fits in L1/L2 cache): NV-HTM beats DUDETM and even SPHT because HTM conflict detection is faster than STM.
- **Very large TXs**: Need STM fallback (DUDETM-style) or SPHT's group-commit approach (which handles large TXs better by amortizing sync cost across many TXs per epoch).

## Implementation (`nvhtm.hpp`)

The implementation follows the standard backend pattern (header + globals + runtime .cpp):

- **Redo log**: Fixed-size (`LOG_CAPACITY=4096`) array of `(addr, type, new_val)` entries in the `Transaction` struct. All writes inside the RTM transaction append to this log AND write-through to memory. The HTM hardware rolls back both on `_xabort()`.

- **Durable phase**: After `_xend()`, `durable_commit()` flushes each log entry cache line via `_mm_clflush`, `_mm_sfence`, then applies all writes to their final addresses. Idempotent — safe to replay on recovery.

- **Pass-through mode**: On CPUs without Intel RTM, `rtm_available()` returns false, `begin()` returns with `active=false`, and all `tm_read_*`/`tm_write_*` operations perform direct memory access with no TM tracking. Multi-threaded correctness is not guaranteed in this mode.

- **Null-address guard**: Writes to addresses `< 0x100000` or with top bit set (kernel space) are silently dropped to prevent SIGSEGV from moved-from null pointer GEPs.

- **Build**: Requires `-mrtm` flag for `_xbegin`/`_xend`/`_xabort` intrinsics.
