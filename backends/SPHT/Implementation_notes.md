# SPHT — Implementation Notes

**Paper**: "SPHT: Scalable Persistent Hardware Transactions" (USENIX FAST 2021)

## Overview

SPHT addresses the **scalability** problem of prior persistent HTMs (including NV-HTM). On real Optane DC PM hardware, prior approaches suffered from:

1. **Synchronous log flushing**: Every TX incurs the latency of clwb + sfence for its redo log, serializing the durable commit path.
2. **Epoch synchronization**: Global epoch barriers force all threads to sync before any can proceed, creating convoying and load imbalance.
3. **Recovery serialization**: Scanning all threads' logs serially at recovery time.

SPHT's contribution is a **group-commit** mechanism that aggregates many transactions' durably-committed writes into a single epoch, amortizing sync overhead and enabling parallel recovery.

## Design

### Per-Thread Commit Log (PCL)
Each thread has an append-only redo log in NVM. Transactions append `(addr, size, value)` entries to the PCL as they execute (inside their HTM transaction).

### Group Commit Protocol

Instead of each TX flushing its log immediately after HTM commit (as NV-HTM does), SPHT defers the flush:

1. **HTM commit** (`xend`): The log entries are atomically visible (in cache), potentially still dirty in the cache hierarchy. No clwb/sfence yet.

2. **Epoch Accumulation**: The PCL continues to accumulate log entries from multiple TXs (the current thread's successful HTM commits). A per-thread counter (`commit_seq`) tracks which TXs are part of this epoch.

3. **Epoch Flush** (when a watermark is reached, e.g. PCL is 75% full or epoch timer fires):
   - Thread A issues clwb for all dirty cachelines in its PCL region.
   - Thread A writes its `commit_seq` to a **durable epoch table** (a small NVM region shared across threads).
   - Thread A calls `sfence`.
   - Now all of Thread A's TXs up to `commit_seq` are durably committed.

4. **Epoch Global Ordering** (optional, for strict ordering guarantees):
   - Before the epoch flush, wait for all other threads to also flush their epochs up to the same global epoch number.
   - After all threads flush, mark the epoch as globally durable.
   - **(Optimization)**: If the application does not require cross-thread ordering (no write-after-read dependencies across threads), step 4 can be skipped.

### Recovery with Parallel Log Replay

1. Each thread's PCL is an independent region. Recovery workers (one per thread) scan and replay each PCL in parallel.
2. The durable epoch table tells each worker which log entries (up to `commit_seq`) are valid. Entries beyond `commit_seq` are stale (the thread crashed before flushing).
3. After all workers finish, the system resumes with clean state.

## Key Data Structures

```cpp
// Per-thread (in NVM, 4MB pre-allocated per thread)
struct alignas(64) PCL { // Per Commit Log
    // Write cursor (touched only by owning thread)
    std::atomic<uint64_t> write_offset;
    // Array of log entries (variable-size)
    char data[PCL_SIZE - CACHE_LINE]; // ~4MB - 64 bytes
};

// Global (in NVM, small — just per-thread sequence numbers)
struct DurableEpochTable {
    // PER_THREAD_MAX entries, written by each thread at epoch flush
    uint64_t commit_seq[PER_THREAD_MAX];
};

// Per-thread, in DRAM (hot path, no NVM writes on fast path)
struct SPHTThreadState {
    uint64_t local_seq;       // incremented per TX
    uint64_t last_flushed_seq;// latest seq flushed to PCL's write_offset
    uint64_t epoch_number;    // monotonically increasing
    PCL *pcl;                 // pointer to this thread's PCL in NVM
};
```

## Comparison to NV-HTM

| Aspect | NV-HTM | SPHT |
|--------|--------|------|
| Log flush | every TX (inline) | group flush (batch) |
| Scalability ceiling | clwb + sfence per TX limits throughput at high thread count | batch amortization allows ~2x higher throughput at 8+ threads (FAST paper reports 1.6x-2.2x at 28 threads) |
| Recovery | serial (one thread scans all logs) | parallel (one worker per thread's log) |
| Log space reclamation | after each TX's apply phase | after epoch is globally durable, the PCL region is reused (circular buffer) |
| Cross-thread ordering | implicit (each TX flushed individually) | explicit via global epoch (optional) |

## Implementation Sketch for clang-tm

```cpp
// PCL region mapped to NVM
// Aligned to cacheline, large enough to hold many TXs before flush
thread_local PCL *g_pcl = /* mmap at startup */;
thread_local SPHTThreadState g_ts = { .local_seq = 0, .last_flushed_seq = 0 };

void sphT_begin() {
    g_ts.local_seq++;
    unsigned status = _xbegin();
    if (status == _XBEGIN_STARTED) return;
    // Retry / fallback logic on abort
}

void sphT_write(void *addr, uint64_t val, size_t size) {
    // Append to PCL (inside HTM)
    uint64_t off = g_pcl->write_offset.fetch_add(sizeof(LogEntry) + size);
    LogEntry *e = (LogEntry*)(g_pcl->data + off);
    e->addr = addr; e->size = size;
    memcpy(e->data, &val, size);
    // Also update DRAM write-set for apply phase
}

void sphT_commit() {
    _xend();

    // Check watermark: flush if PCL is >75% full or at epoch boundary
    if (g_pcl->write_offset > WATERMARK) {
        // 1. Flush PCL cachelines to NVM
        for (uint64_t off = 0; off < g_pcl->write_offset; off += 64)
            _mm_clwb(g_pcl->data + off);
        // 2. Update durable epoch table
        g_epoch_table->commit_seq[thread_id] = g_ts.local_seq;
        _mm_sfence(); // Make checkpoint visible before apply

        // 3. Apply writes (from g_pcl, up to write_offset)
        apply_pending_writes(g_pcl);

        // 4. Reset PCL for next epoch (circular)
        g_pcl->write_offset = 0;
        g_ts.last_flushed_seq = g_ts.local_seq;
    }
    // else: defer flush — writes are still in cache, not yet durable
}
```

## Limitations

1. **Loss window**: TXs that committed (HTM commit) but were not yet flushend (epoch boundary not reached) are lost on power failure. The epoch table tells recovery they did not durably commit. If the application requires every TX to be durable immediately, SPHT's deferral is unacceptable.

2. **Epoch timer tradeoff**: A shorter timer reduces the loss window but increases flush frequency (reducing the amortization benefit). The paper uses a 10µs timer as the default.

3. **NUMA awareness**: The PCL must be mapped on the local NUMA node's NVM for optimal write latency. Cross-NUMA PCL accesses for the epoch table are a known bottleneck (paper discusses NUMA-aware epoch table partitioning).

4. **Apply phase ordering**: The apply phase (writing values to their final addresses) must be ordered after the epoch table flush (sfence) so that recovery sees consistent state. If apply crashes mid-way, the epoch is still recoverable because the PCL has all entries.

## When to Use

- **High-throughput workloads** (banking, graph processing) where a small loss window is acceptable.
- **Large TXs** that would overflow NV-HTM's single-TX PCL capacity — SPHT handles this naturally by batching across multiple HTM TXs per epoch.
- **Multi-socket Optane PM systems** where NV-HTM's single-thread-per-TX model does not scale.

## Implementation (`spht.hpp`)

The implementation follows the same pattern as NV-HTM but with a per-thread commit log (PCL) shared across multiple TXs:

- **PCL**: A `std::vector<LogEntry>` per thread (`g_pcl`), appended to on each `tm_write_*`. Entries accumulate across multiple HTM transactions within an epoch.

- **Group commit**: Every `GROUP_COMMIT_INTERVAL=16` non-read-only TXs, `group_commit()` flushes the entire PCL via `_mm_clflush` + `_mm_sfence`, publishes the durable TX sequence number to a global epoch table, then applies all writes. This amortizes the flush cost across 16+ TXs.

- **Epoch table**: `g_durable_seqs` is an array of `std::atomic<uint64_t>` (one per thread). Each thread publishes its TX sequence number after each group commit. Recovery scans for the minimum across all threads to determine the last globally-durable point.

- **Pass-through mode**: Same as NV-HTM — on non-RTM CPUs, `begin()` returns with `active=false` and all operations bypass TM tracking.

- **Null-address guard**: Same as NV-HTM — addresses `< 0x100000` or kernel-space are silently skipped.

- **Build**: Requires `-mrtm` flag for RTM intrinsics.

## Key Results from Paper

- 1.6x-2.2x throughput improvement over NV-HTM at 28 threads (real Optane DC PM).
- Write latency: ~480ns (SPHT) vs ~700ns (NV-HTM) for durable commit.
- Recovery: 0.8s for 64GB log (16 workers in parallel) vs ~12s for serial replay.
- Loss window at 10µs timer: ~10µs worth of TXs (configurable).
