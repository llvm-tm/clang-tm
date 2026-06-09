# XTM — eXtended Transactional Memory (ASPLOS 2006)

## Reference

J. Chung, C. Cao Minh, A. McDonald, T. Skare, H. Chafi, B. D. Carlstrom,
C. Kozyrakis, K. Olukotun. *Tradeoffs in Transactional Memory Virtualization.*
ASPLOS 2006.

## Core Idea

XTM virtualizes all three aspects of transactional execution (time, space,
and nesting depth) using a **software-only, page-granularity** approach built
on virtual memory primitives.  It requires no special hardware support.

## Key Data Structures (from the paper)

| Structure | Name | Role |
|-----------|------|------|
| **XSW** | Transaction Status Word | Per-thread state (ACTIVE / COMMITTED / ABORTED) |
| **XADT** | Transaction Address/Data Table | Global hash table keyed by page address; each entry holds the current version number and the owning transaction ID |
| **XF** | XADT Filter | Bloom filter for fast negative lookups (avoid XADT probe when a page is definitely free) |

## Algorithm

### Memory model

TM-allocated memory is tracked at **4 KB page** granularity.  When a
transaction first writes to a page it:

1. Acquires ownership via `CAS` on the XADT entry (owner_tx_id = 0 → tx_id).
2. Creates a **private copy** of the full page via `memcpy`.
3. Sets the XF bloom-filter bit.
4. Subsequent writes within the same transaction go to the private copy.

Reads take a **snapshot** of the page's version from the XADT entry and
record it in the read set.

### Conflict detection

- **Write–write**: The CAS on the XADT owner field fails if another
  transaction already owns the page → the second writer aborts.
- **Write–read**: A reader records the page version (`g_xadt[idx].version`)
  at first access.  At commit time it validates that no version has changed
  for any page in its read-set (unless the page is in its own write-set).
- **Read–write**: If a reader sees `owner_tx_id != 0` and `!= own id`, it
  aborts immediately (eager conflict detection for reads of owned pages).

### Commit protocol

1. **Validate** — for every page in the read-set that is NOT also in the
   write-set, check that `g_xadt[idx].version` matches the recorded snapshot.
   If any mismatch → abort.
2. **Write-back** — for every page in the write-set, `memcpy` the private
   copy back to the original page, bump the XADT version, release ownership.

### Abort

Release ownership of all write-set pages (clear `owner_tx_id`), free private
copies, and `siglongjmp` to the `tm_begin()` checkpoint.

### Nesting

Nested transactions are handled by the runtime's counter (`tm_nested_call_counter`):
the inner `tm_begin()` increments the counter and returns immediately without
creating a new XTM transaction; the outer `tm_commit()` performs the real
commit only when the counter reaches 1.

## Differences from the paper

- The paper's XTM targets **hybrid TM** (HTM + software fallback).  This
  implementation is a pure-STM adaptation that always runs the XTM page-level
  protocol.
- The paper uses `mprotect` + SIGSEGV handlers for conflict detection.
  This implementation uses **polling-based** checks on the XADT (`owner_tx_id`
  field and version counters), which avoids signal-handler complexity but
  adds a software check on every TM read/write.
- The Bloom filter (XF) is optional in the paper; we include it as a cheap
  negative indicator to skip XADT probes when the page is clearly free.
- The paper describes multi-version support for snapshot isolation; this
  implementation uses single-version pages with optimistic validation.

## Build

XTM is built as part of the `tinystm` (or whichever) build target.
No special compiler flags are required beyond `-std=c++17`.
