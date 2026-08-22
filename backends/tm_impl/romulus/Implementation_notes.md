# Implementation Notes: Romulus (version-table OCC)

## Naming caution

Despite the directory name, this backend is **not** the persistent Romulus of
Correia, Felber, and Ramalhete (SPAA 2018), "Romulus: Efficient Algorithms for
Persistent Transactional Memory". That paper describes persistent objects with
copy-on-write roots and a recovery sweep; this backend is a **volatile,
shared-memory, version-table OCC** design that merely borrows the name. See
`docs/book` Chapter 18 for the disambiguation.

## Overview

A single-global-clock OCC STM:

- Writes are buffered in a per-transaction write-set (no in-place mutation,
  no undo log).
- Reads go through a **version table** and a read-set with read-validate.
- Commits serialize on one spinlock, write back, then publish new versions.

## Core data structures

| Structure | Purpose |
|-----------|---------|
| `g_version_table[]` | 2^20 entries of `atomic<uint64_t>`, indexed by `(addr >> 3) & mask`. Independent of data addresses — the table is *not* overlaid on user memory. |
| `g_commit_lock` | Spinlock serializing the commit path. |
| Per-tx read/write sets | Write-set buffers values; read-set records `(addr, observed_version)`. |

## Protocol

### Begin

Copy the global clock into `tx->timestamp` (the snapshot version).

### Read (`read_word`) — read-validate

1. Look up the local write-set first (read-own-writes).
2. Load the version-table entry **before** reading data; abort if the lock
   bit is set.
3. Read the value from memory.
4. Re-load the entry **after** the read; abort if it changed (version bump or
   lock acquisition during the read).

The re-check closes the classic OCC hole: without it, a concurrent commit's
write-back can land between the version check and the data load, and the
reader silently assembles an inconsistent snapshot that end-of-transaction
validation cannot detect (this was a real multi-threaded bank failure,
fixed 2026-06-15).

### Commit

1. Validate: every read-set address's current version ≤ `tx->timestamp`.
2. Acquire `g_commit_lock`; re-validate (writers serialize here).
3. Increment the global clock → `commit_ts`.
4. Write back the whole write-set.
5. Fence.
6. Store `make_version_entry(commit_ts)` into each written address's table
   entry.
7. Release the lock.

## Verification status

- `test_tx`: 114/114 PASS
- `test_ds`: 207/207 PASS
- `bank -t 4 -d 500 -a 128`: money conserved (after the read-validate fix)

## Trade-offs

- Validation cost grows with read-set size (full re-validation at commit).
- The commit lock serializes all writers — throughput ceiling under
  write-heavy contention.
- No persistence: everything lives in volatile memory.
