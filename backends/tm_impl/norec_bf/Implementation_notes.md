# NOrec-BF: NOrec with Bloom-filter fast validation

NOrec-BF is NOrec (Dalessandro et al., 2010) with its value-based validation
replaced by a **Bloom-filter disjointness fast path**.  The core algorithm
(global sequence lock, invisible readers, write-set logging, value-based
validation) is unchanged; NOrec-BF only accelerates the `validate()` step that
NOrec runs on every read after the clock moves and at every writer commit.

## Motivation

NOrec's `validate()` re-reads the *entire* read set from memory and compares
each value against the observed one.  For transactions with large read sets
(scan-style workloads, read-mostly transactions), this is a long memory
traversal performed under a repeating snapshot check.

A Bloom filter represents a set of keys in a fixed bit array.  Two properties
make it the right structure for this problem:

1. **No false negatives** — if a key is in the set, every probe hits a set bit.
   Consequently an empty bit-intersection between two filters proves the sets
   are **definitely disjoint**.  A "disjoint" answer is always exact.
2. **Constant-time disjointness** — whether *any* element of one set appears in
   the other is answered by AND-ing the two fixed bit arrays: `O(WORDS)` word
   operations, independent of set sizes.  (Exact O(1) intersection is
   information-theoretically impossible; the filter trades false positives for
   constant time.)

A non-empty intersection is only a *maybe* (false positive), so callers fall
back to an exact check; a disjoint answer short-circuits validation soundly.

## Design

NOrec-BF adds two structures:

- **`g_gc`** — a global *committed-writes* Bloom filter (`stm::BloomFilter<64>`,
  4096 bits, k = 2 splitmix64 hashes).  Every committing writer inserts the
  addresses of its write set **under the global sequence lock**, then releases
  the lock.  This publication under the lock means no reader can observe a
  half-updated filter (a reader's clock double-check retries if the lock was
  taken while it read `g_gc`).
- **`tx->read_bf`** — a per-transaction *read-set* Bloom filter, populated on
  every read, plus **`tx->gen`** — the `g_gc_gen` generation recorded at
  `begin()`.

**`validate()` fast path** (before the exact re-read loop):

```
time = clock (acquire);            spin if odd
if gen unchanged since begin()
   AND tx->read_bf.disjoint_with(g_gc):
       if time == clock: return time   // sound: no committed writer touched
       continue                        // any read address since last rebuild
```

The clock double-check (the same acquire/re-acquire pattern NOrec already uses
around value re-reads) brackets the filter read, so a concurrent commit or
rebuild is always detected and retried.  Because `g_gc` inserts happen-before
the writer's release store, and the reader's clock reads bracket the filter
read, observing a commit in `g_gc` forces the clock to differ on re-read.

**Saturation control (the generation).**  `g_gc` accumulates every committed
write, so its false-positive rate rises until the disjoint test almost always
fails.  When `g_gc` is ~50% full, a writer (under the lock) bumps `g_gc_gen`
and clears the filter.  An in-flight transaction whose `tx->gen` no longer
matches takes the exact path — it can never be misled by a filter that was
rebuilt after it began.  Fresh transactions get a clean filter.

### Why this is sound

- The disjoint test has **no false negatives**: if a committed writer wrote any
  address in the read set, that address's bits are set in both filters and the
  AND is non-zero → the fast path is skipped.  So "disjoint" ⇒ no committed
  write to any read address since the last rebuild.
- If the generation matches, the last rebuild predates `begin()`, so the filter
  contains every write committed after `begin()` (plus conservative pre-begin
  writes, which only cause false positives → exact path).  Hence "disjoint" ⇒
  the read set is unchanged since the transaction's snapshot ⇒ validation
  passes without touching memory.
- If the filter was rebuilt mid-flight, the generation differs → exact path.

## Verification

| Test | Result |
|------|--------|
| `test_tx` | 114/114 PASS |
| `test_ds` | 207/207 PASS |
| `fuzz_counter -t4 -n1000 -c8` | PASS (counter sum conserved) |
| `fuzz_bank -t4 -n1000 -a64` | PASS (money conserved) |
| `bank -d 500 -a 128 -t 4` | PASS (money conserved) |

### Performance

Apple M1 Pro, `bank -d 2000 -a 512 -t 4 -r 90` (90% read-mostly, read-all
scans 512 accounts):

| Backend | Txns/sec |
|---------|----------|
| NOrec   | 552,901 |
| NOrec-BF | 829,304 |

**+50%** on the read-mostly scan workload: a 512-address read set is validated
by 64 word-ANDs instead of 512 memory reads.  On small read sets (bank
transfers read ~2 addresses) the fixed 64-word scan costs slightly more than 2
direct reads, so NOrec-BF is marginally slower there (~7% on `-r 0` transfers)
— the fast path pays off when the read set is large relative to `kBloomWords`.

## Tuning

- `kBloomWords` (in `NOrec_BF.hpp`): filter width in 64-bit words.  64 words =
  4096 bits ≈ 2048 keys at the 50% rebuild threshold.  Larger → fewer false
  positives but a more expensive disjoint test; smaller → cheaper test but more
  frequent rebuilds.
- Rebuild threshold is fixed at 50% bit density in `commit()`.
