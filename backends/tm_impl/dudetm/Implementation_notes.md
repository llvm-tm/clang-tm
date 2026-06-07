# DUDETM — Implementation Notes

**Paper**: "An Analysis of Dude TM: Decoupled Durable Transactions" (ASPLOS 2017)

## Overview

DUDETM (Decoupled and Deferred — originally DUrable + DEferred TM) decouples the **persistence** of transactional writes from the **atomic visibility** of the STM commit. Instead of making Durability part of the TM commit protocol, DUDETM treats durability as a background I/O concern that can be overlapped with forward progress.

## Key Idea: Three-Phase Model

1. **Perform** — The STM transaction runs normally (read-set, write-set, validation, commit). The TM commit makes writes atomically visible to other threads (e.g. by version-clock publish or lock release). No persistence barrier at this point.

2. **Persist** — After the TM commit, a background mechanism flushes the write-set's redo log to NVM. This can be overlapped with subsequent transactions. The log must contain enough metadata (address, length, value) to replay the write on recovery.

3. **Reproduce** — After a crash, the recovery process replays the persisted log entries, applying each write to its target address. This re-establishes the durable state that had been committed by the TM but not yet reflected in the primary NVM locations.

## Core Mechanisms

### Write Set Logging
Each transaction appends its writes to a **redo log** in NVM:
- `(tx_id, addr, length, new_value)` per write
- Log is non-temporal (write-combining / clwb) so log writes are visible after a power failure

### Log Truncation
After a log entry is flushed to NVM **and** the corresponding primary-memory location has been updated (either by the same transaction's write-back or by a later transaction's write-back), the log entry can be truncated. DUDETM proposes:
- **Per-thread log**: natural truncation point is after the last committed-but-not-persisted write
- **Global log with epoch counters**: truncation when all threads confirm persistence up to an epoch boundary

### Recovery Protocol
1. Scan the redo log for the last complete set of per-transaction write entries
2. For each complete transaction, apply all writes (addr <- value)
3. Truncate all applied entries
4. Resume normal operation

## Interface Points with clang-tm

At the plugin level, DUDETM looks identical to any write-back TM (WBCTL, TL2, NOrec) — the same `tm_read_*`/`tm_write_*` hooks are emitted. The difference is entirely inside the hooks:

### Required Hook Changes

| Hook | What DUDETM adds |
|------|-----------------|
| `tm_write_*` | Append `(addr, size, val)` to redo log in NVM (`clwb` + `sfence`) |
| `tm_commit` | STM commit (visibility) THEN async-flush signal to background thread |
| `tm_abort` | Discard write-set (no NVM action needed) |
| `tm_begin` | Allocate per-TX log buffer, register with background flusher |

### Background Flusher Thread
- Polls a `std::atomic<bool>` flag per active TX
- When flag is set (by `tm_commit`), flushes the TX's redo log entries from write-combining buffer to NVM
- After flush, marks the TX as "persisted" so its log can be truncated

## Current Skeleton (`dudetm_base.hpp`)

- `#pragma once`, includes `<atomic>`, `<cstdint>`, `<cstdio>`
- No implementation yet — needs:
  - `TxDescriptor` with `redo_log` (vector of `(void* addr, size_t size, uint64_t val)`)
  - `flush_log()` — issues `clwb` + `sfence` for each log entry (or uses non-temporal stores)
  - `background_flusher` thread function
  - Recovery entry point (`recover_from_log()`)

## Limitations

1. **STM commit is still blocking**: Visibility (TM commit) must be ordered before the persist signal (otherwise a reader could see a write that hasn't even reached the log). This means the critical commit path is: STM commit → signal → return. The "decoupling" only means the *flush to NVM* is async, not the *log append*.

2. **Log space**: Without truncation during normal operation, the log grows unbounded. A watermark-triggered truncation pass (scanning for committed-and-persisted entries) is needed.

3. **STM write-set is in DRAM**: `tm_write_*` must write to both the in-DRAM write-set (for STM commit resolution) AND the NVM redo log (for durability). This doubles the per-write overhead.

4. **clwb + sfence portability**: clwb is available on Intel Cascade Lake+; AMD uses `clwb` or `clzero`; on non-x86 platforms, platform-specific flush instructions are needed.
