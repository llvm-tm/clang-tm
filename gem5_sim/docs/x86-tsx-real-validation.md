# gem5 X86_TSX vs real Intel TSX — validation on intel14v2 (2026-10-05)

Real machine: `intel14v2` — Intel Xeon E5-2660 v4 (Broadwell-EP, 2.0 GHz base,
`rtm`+`hle`, microcode current), Debian 12, gcc 12.2.
Simulator: local gem5 `v25.1.0.1` + repo patch set (`gem5_sim/setup.sh`,
target `X86_TSX`), board from `configs/x86-se-bank.py`
(Ruby `MESI_Three_Level_HTM`, `TimingCPU` unless noted, clk 1.8 GHz, DDR3-1600).
Both sides run **identical sources** from this working tree; guest binaries
are static x86-64 (`-mrtm -pthread -static`).

## 1. Conflict-detection probes (`benchmarks/tsx`)

`tsx_conflict_matrix.c` (real) and `tsx_conflict_matrix_gem5.c` (gem5 SE
mirror; iteration count as argv, pins 1/2; SE ignores `sched_setaffinity` but
SE `clone` places the two workers on distinct cores — confirmed via the
per-core Ruby HTM counters, which attribute every abort to the reader core).

Ground truth re-verified on intel14v2 2026-10-05; matches 2026-08-29 baseline
(see `benchmarks/tsx/ground_truth_intel14v2.txt`, both passes appended).

| mode | real HW: abort% (reader) | real HW: abort% (writer) | gem5 timing | gem5 o3 |
|------|-------------------------|--------------------------|-------------|---------|
| RR   | 0.0 | 0.0 | 0.0 / 0.0 | – |
| RW   | 65.8 / 69.1 (2 passes) | 0.4 | 2.9 (reader only) | 3.2 (reader only) |
| WR   | 43.5 / 50.1 | 0.1–0.6 | 2.9 (reader only) | – |
| WW   | 0.0–0.2 (one pass: 11.2 asymmetric) | | 0.0 / 0.0 | 0.0 / 0.005 (1 abort) |

**Qualitative verdict: match.** Reads never conflict; in RW/WR the *reader*
always aborts and the writer commits; WW conflicts are rare for 1-access
transactions and appear (1 abort) under the less-deterministic o3 CPU,
matching real HW's occasional asymmetric collisions.

**Quantitative gap:** gem5's overlap *rate* is ~20× lower (2.9% vs ~50-70%).
Cause: the timing CPU makes both free-running threads near-deterministic, so
their phases barely drift and the windows where both touch the line at once
are rare; real hardware's interrupt/cache jitter keeps re-randomizing the
phase. gem5 sees conflict *semantics* correctly, not the *arrival process*.
For fidelity of abort *rates* (not just outcomes), drive overlap explicitly
(longer transactions or staggered starts) rather than trusting free-run.

Spurious aborts: real HW 0.0006%/tx (1M single-thread, unchanged by thrash
threads); gem5 SE is exactly 0 (no interrupts/SMIs/timers modeled). Structural
difference; any simulator with 0% spurious slightly over-predicts long-running
transaction throughput (see `benchmarks/tsx/README.md`).

## 2. Bank benchmark throughput + abort rates (`benchmarks/cpp/bank`)

`bank BACKEND=TSXSGL` (same source both sides), `-a 64 -r 20`, quota `-n`.
All runs on both sides print `PASS: Money conserved`.
Abort mixes from `TM_TSX_STATS=1` (counters added to
`TSXSGL_runtime.cpp` this session; per-thread, merged outside transactions —
a *shared* atomic incremented inside a live TX makes the counter line itself
a conflict source and inflated aborts ~2×, caught mid-session).

### Real HW (2.0 GHz, gcc 12.2 -O3)

| run | Txns/sec | xbegin | TSX commits | conflict aborts | SGL entries |
|-----|---------:|-------:|------------:|----------------:|------------:|
| t=1 n=20000 | 6.67 M | 20000 | 20000 | 0 | 0 |
| t=2 n=20000 | 2.22 M | 51440 | 14766 | 33026 (64%) | 5234 |
| t=4 n=20000 | 1.25 M | 75328 | 9362 | 53469 (71%) | 10638 |
| t=1 -d 1000 | 8.10 M | 8101033 | 8100896 | 0 | (137) |
| t=4 -d 1000 | 2.50 M | 5813196 | 1194085 | 3637969 (63%) | 670093 |

### gem5 (Timing CPU, 1.8 GHz, simulated time)

| run | Txns/sec (sim) | xbegin | TSX commits | conflict aborts | SGL entries |
|-----|---------------:|-------:|------------:|----------------:|------------:|
| t=1 n=20000 | 0.87 M | 20000 | 20000 | 0 | 0 |
| t=2 / t=4 | see note | | | | |

**t=2/t=4 blocked — known issue, not new:** 2+-thread gem5 runs on the
`MESI_Three_Level_HTM` hierarchy hit the documented Ruby livelock (TODO.md
P1 "gem5 multi-threaded livelock", review-02 S25; affects all backends).
Reproduced 2026-10-05 on a fresh v25.1.0.1 build with the **pristine**
(pre-session) runtime binary: t=2 `n=100` unfinished after 4 min wall;
`n=2000` unfinished after 6.7 min. `n=1` completes cleanly (7 min wall at
`n=20000`). The 2026-09-05 sweep's t=2/t=4 rows were produced by
`run_gem5_sweep.py`'s automatic tick guard (`--mt-ticks`, default 2e9 ticks
= 1.1 ms simulated) and are **partial** ROI samples, not completed runs.
The conflict probes in §1 are 2-thread runs and *do* complete because their
working set is one line and their duration is short; the bank's 64-account
traffic reliably triggers the livelock. A real fix (HTM logic on a
livelock-free hierarchy) is tracked in `TODO.md`; until then gem5-side
contended-bank abort-rate fidelity cannot be measured beyond tick-capped
samples.

Tick-capped September samples (`x86-tsx-validation.md` §4; pre-fence design,
n=2000, `--mt-ticks 2e9`): t2 1213 xbegin / 609 aborts (50%), t4 1617 / 1611
(**99.6%** aborts) — partial-ROI counts, shown for shape only. Real HW
reaches ~64–71% conflict-aborted *attempts* at t2/t4; gem5's t4 sample
over-aborts because every SGL entry writes the `sgl_owner` line and kills
all in-flight TSX transactions — amplification the earlier report already
flagged as *real in both* designs, though its magnitude differs (gem5 has no
backoff jitter).

t=1 per-transaction cost: gem5 ~2071 cyc/txn vs real ~300 cyc/txn (2 GHz);
consistent with the existing Broadwell-EP calibration notes (fixed 60c start +
178c commit + body, Timing CPU adds per-mem-op latency). gem5's t=1 abort-free
mix matches real t=1 exactly (20000/20000/0).

## 3. Key finding: a real-HW race gem5 cannot see

First-ever hardware fuzz of `tsx_sgl` (`fuzz_counter` 4t on intel14v2)
**lost updates 12/12 runs** (plus 2/3 at 2t) — while the same binary passed
every gem5 run.
Root cause: the SGL fallback's `sgl_owner=1` plain store stayed in the core's
store buffer while it read/wrote shared data; a concurrent TSX transaction
that had loaded `sgl_owner==0` committed, then the stale-based SGL stores
landed after it. gem5's Ruby HTM model commits at coherence granularity with
no store-buffer visibility window, so this entire race class is invisible to
it. Fixed with an entry-side `mfence` (ablation: 8/8 LOST before, 8/8 PASS
after at 4t/800k RMW; post-fix fuzz/bank suites pass 10/10). Details:
`docs/CORRECTNESS_FIXES.md` §19.

Implication for the validation tiering in `x86-tsx-validation.md` §5: real
hardware catches *memory-model* bugs that gem5 structurally cannot; gem5
validates *protocol outcomes* (who aborts, money conservation under coherent
interleavings, cycle budgets), not store-buffer timing races.

## 4. Reproduce

```sh
# gem5 build + bank (this repo, x86-64 host)
./gem5_sim/setup.sh                       # clone+patch+build X86_TSX
./gem5_sim/scripts/run-bank-gem5.sh TSXSGL 1 64 20000   # also 2, 4
# conflict probes under gem5 SE (3-core board, 20k iters/mode)
make -C benchmarks/tsx gem5
for m in RR RW WR WW; do
  gem5_sim/gem5/build/X86_TSX/gem5.opt -d /tmp/p-$m gem5_sim/configs/x86-se-bank.py \
    --binary $PWD/benchmarks/tsx/bin/tsx_conflict_matrix_gem5 --threads 2 \
    --max-ticks 400000000000 --raw-args "$m 20000"
done
# real hardware
ssh intel14v2 'cd ~/clang-tm/benchmarks/tsx && make && ./tsx_conflict_matrix RW'
ssh intel14v2 'cd ~/clang-tm/benchmarks/cpp && make bin/bank BACKEND=TSXSGL && \
  TM_TSX_STATS=1 ./bin/bank -t 4 -a 64 -r 20 -n 50000'
```
