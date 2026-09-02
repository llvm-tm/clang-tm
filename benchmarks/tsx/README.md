# TSX Ground-Truth Probes

Two tiny standalone programs that capture the *real* behaviour of Intel TSX
(RTM) on whatever machine they are run on.  They are the oracle against which
the TSX simulator (`explicit_api/rust/workspace/runtime/tsx_sim/`) must be
calibrated and validated.

## Why these exist

The TSX simulator currently models conflict detection as "the committer wins
and aborts everyone who read/wrote a shared line".  Real TSX resolves
contention through the cache-coherence protocol, so:

1. **Who aborts depends on the access schedule**, not just on who reaches
   `xend` first.  A read-write conflict may abort the reader, the writer, or
   both, depending on the order the two accesses hit the line.

2. **Spurious aborts happen.**  Even a completely contention-free, write-free
   transaction aborts occasionally (timer interrupt, SMI, cache eviction,
   sibling-hyperthread L1-set pressure).  A simulator with zero spurious
   aborts over-predicts throughput, sometimes by a lot.

The two programs below measure exactly these two properties.

## Programs

### `tsx_conflict_matrix.c`
Two threads pinned to different physical cores (1 and 13 on a 14-core
Broadwell) free-run 200k iterations each: `xbegin -> single access -> xend`
on the *same* cache line, with one outer barrier to synchronize start.  No
per-iteration handshake is used (a per-iteration barrier or a flag handshake
*inside* TX causes a false RW conflict on the handshake line itself and yields
100% aborts).  Transactions are kept short (one access) so the rate reflects
conflict semantics, not timer-interrupt time-outs (a 2000-`pause` TX aborts
~100% spuriously on this hardware).

### `tsx_spurious.c`
One pinned thread runs millions of isolated single-line read-only transactions
and reports the abort rate.  Optional background "thrash" threads force cache
coherence traffic to reveal eviction-driven spurious aborts.  This is the
*spurious-prevalence* oracle.

## Building (on a TSX-capable machine, e.g. intel14v2)

```sh
gcc -O2 -mrtm -pthread -o tsx_conflict_matrix tsx_conflict_matrix.c
gcc -O2 -mrtm -pthread -o tsx_spurious       tsx_spurious.c
# or
make -C benchmarks/tsx
```

## Running

```sh
./tsx_conflict_matrix RR
./tsx_conflict_matrix RW
./tsx_conflict_matrix WR
./tsx_conflict_matrix WW

./tsx_spurious 1000000        # baseline spurious rate
./tsx_spurious 1000000 4      # with 4 background thrash threads
```

## Ground truth on intel14v2 (E5-2660 v4, Broadwell-EP, 2026-08-29)

All runs pinned 1/13 (different physical cores), `gcc 12.2 -O2 -mrtm`,
`200k` iters per thread for the matrix, `1M` for spurious.  Raw logs are in
`ground_truth_intel14v2.txt`.

### Conflict matrix (short TX, free-running, pinned 1/13)

| Mode | T1 (first char) | T2 (second char) | Result |
|------|-----------------|------------------|--------|
| `RR` | R 0.0% abort (0/200k conflict) | R 0.0% abort (0) | both commit — reads never conflict |
| `RW` | R 58-68% abort (conflict) | W 0.1% abort | **reader aborts, writer wins** |
| `WR` | W 0.1% abort | R 62-66% abort (conflict) | **reader aborts, writer wins** |
| `WW` | W 0.1-0.2% abort | W 0.1% abort | rarely overlaps for a 1-access TX; need a longer TX to force overlap |

Representative run (`200k` iters):

```
RR: T1 R 199991/9 (0.0%)  T2 R 199998/2 (0.0%)
RW: T1 R  83874/116126 (58.1%, 116097 conflict)  T2 W 199874/126 (0.1%)
WR: T1 W 199784/216 (0.1%)  T2 R 75748/124252 (62.1%)
WW: T1 W 199680/320 (0.2%)  T2 W 199798/202 (0.1%)
```

Interpretation: with free-running 1-access TXs the overlap window is small,
so even contending modes show <70% reader aborts (the rest commit without
overlapping).  Forcing overlap (e.g. with a 1000-`pause` delay inside TX)
would raise RW/WR abort rates toward 100% for the reader.

### Spurious aborts (1M single-thread, no contention)

```
baseline (0 thrash):  4-6 aborts / 1M  (0.0004-0.0006%)
4 thrash threads:     5-9 aborts / 1M  (0.0005-0.0009%)
```

Timer interrupts and cache evictions are the dominant spurious sources;
background thrash barely moves the rate on this machine.

## How to use for simulator validation

1. Run both probes on the target machine, store the per-thread commit/abort
   counts and abort-reason breakdown.
2. Run the same schedule through `runtime/tsx_sim` (via `simulator/` with
   `--backend tsx-sim --machine-profile broadwell_ep_v4.json`) and compare:
   * RR must be ~0% abort for both; RW/WR must abort the *reader*; WW
     should show one writer winning when overlap is forced.
   * Single-thread abort rate must be ~0.0004% (not 0%).
3. Tune `TSX_SIM_MAX_READ_LINES` / `MAX_WRITE_LINES` and bloom-filter
   parameters until the simulated matrix matches the ground truth within
   measurement noise.
