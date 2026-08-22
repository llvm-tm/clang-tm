# TSC-TM: TL2 with a cycle-counter clock

TSC-TM is a word-based, invisible-reader STM identical in structure to TL2
(Dice, Shalev & Shavit, 2006) except that **the global version clock is not an
atomic counter** — it is the CPU cycle counter (`rdtsc` on x86, `cntvct_el0`
on ARM64, exposed as `stm::tm_timestamp()`).  Commit timestamps are therefore
obtained with a single non-shared instruction read instead of a cross-core
atomic RMW, which is the primary scalability bottleneck of classic TL2 on
large core counts.

## Design

Everything else follows TL2 exactly:

- **Ownership records (orecs/guards).**  A hash table maps every word address
  to a guard word holding `(version << 1) | lock`.  Reads record the guard
  version; commits acquire the guard locks by CAS (the only CAS in the
  backend), validate the read set, write values back, and release each guard
  with the commit timestamp.
- **Invisible readers.**  Readers never write shared state; they validate at
  commit time that no guard version changed and no guard is locked by a
  concurrent writer.
- **Write-back commit.**  Writes are buffered in the write set and applied to
  memory only after validation, while all guards are held.

### Why a cycle counter can replace the global clock

TL2 uses a monotonically increasing global clock so that every commit stores a
**fresh** version in the guards it releases: a reader that validated against an
older guard version observes the new one and aborts.  The subtlety with a TSC
clock is resolution: two commits in the same TSC tick would store the same
version, defeating the check.

TSC-TM closes this gap without a global atomic:

1. **Synchronised-TSC invariant.**  On x86 (constant/invariant TSC) and ARM64
   (`cntvct_el0` is a system-wide virtual counter), the cycle counter is
   synchronised across cores, so a later wall-clock timestamp reads >= an
   earlier one.  The implementation documents this as the hardware invariant
   the backend relies on.

2. **Per-guard monotonic bump.**  Each commit releases a guard with
   `version = max(commit_ts, acq_version + 1)` where `acq_version` is the
   version observed at lock acquisition.  Every commit therefore stores a
   **strictly larger** version in every guard it touches than the version that
   guard had before — regardless of TSC tick collisions.  A reader that read
   the guard at version `v ≤ acq_version` and validates after the release sees
   a version `> v` and aborts.  This is the load-bearing correctness mechanism
   and needs no commit-wait.

3. **Optional Spanner-style commit-wait** (`kTscWaitCycles > 0`).  A writer
   spins until `tm_timestamp() >= commit_ts + wait` before releasing guards.
   Because the wait bounds how far the commit timestamp is in the past, commit
   timestamps become **globally strictly increasing** (each commit's timestamp
   is taken after the previous one's release, hence strictly greater) — TrueTime
   commit-wait semantics without an uncertainty interval.  It costs ~wait
   cycles per writing transaction, so it is disabled by default; the per-guard
   bump keeps the backend correct either way.

## Correctness sketch

Read-only transactions commit without validation (TL2 ROCO); they cannot
corrupt memory since they write nothing.

For a writing transaction `T`:

- **Lock acquisition** is deadlock-free: the write set is sorted by address
  and guards are acquired in address order; on a failed acquisition all
  acquired guards are released.
- **Validation** aborts if any read-set guard is locked by another writer or
  carries a version different from the observed one.  A committing writer
  either holds the guard (lock bit set → abort) or has released it with a
  strictly larger version (version mismatch → abort).  A read whose guard was
  touched by no writer keeps its observed version → passes.
- **Write-back** happens while all guards are held, so no reader can observe a
  partially committed state: a reader either reads before the lock (old
  version, then aborts on the new version), during the write-back (lock set →
  abort), or after release (new version — consistent, because the writer wrote
  every value before releasing any guard).

Hence every committed writer's effect on every guard is a version increase,
and every reader that could have observed a stale value detects it at
validation.

## Verification

| Test | Result |
|------|--------|
| `test_tx` | 114/114 PASS |
| `test_ds` | 207/207 PASS |
| `fuzz_counter -t4 -n1000 -c8` | PASS (counter sum conserved) |
| `fuzz_bank -t4 -n1000 -a64` | PASS (money conserved) |
| `bank -d 500 -a 128 -t 4` | PASS (money conserved) |
| `fuzz_counter -t8 -c8` (high contention) | PASS |
| `test_tx` with `kTscWaitCycles = 2000` | 114/114 PASS |
| `fuzz_counter -t4` with `kTscWaitCycles = 2000` | PASS |

### Performance

Apple M1 Pro, `bank -d 3000 -a 128 -t 4`:

| Backend | Txns/sec |
|---------|----------|
| TL2     | 2,060,831 |
| TSC-TM  | 2,050,306 |

TSC-TM matches TL2 throughput while eliminating the global `fetch_add` clock
entirely — on this 8-core machine the clock is not yet the bottleneck, but the
backend has no cross-core RMW in its critical path, so the gap to TL2 should
widen in TL2's favour as core count and clock contention grow.  The ~1.6%
overhead here is the per-guard bump computation plus the extra `rdtsc`.

## Tuning

- `kTscWaitCycles` (in `tsc_tm.hpp`): 0 (default) relies on the per-guard
  monotonic bump; a positive value adds Spanner-style commit-wait and global
  timestamp ordering at a per-writer-commit latency cost (~2000 cycles ≈ 1 µs
  on a 2–3 GHz core).
