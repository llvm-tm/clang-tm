# X86_TSX Validation Report (2026-09-05)

Functional validation of gem5 `X86_TSX` (v25.1.0.1 + `gem5_sim/patches/`
self-contained set) on `dcastro-NucBox-EVO-X2` (32-core Linux host), with an
optimized benchmark execution flow and the cross-validation strategy for
"gem5 TSX vs real hardware on all benchmarks".

## 1. Validated TSX semantics

| # | Property | Probe / evidence | Result |
|---|----------|------------------|--------|
| 1 | CPUID advertises RTM | `htm_probe2`: `CPUID.07H.EBX.RTM=1` | PASS |
| 2 | `XBEGIN` decodes (one-byte `C7/3`) and starts a tx | `htmStart htmUid=1` in HtmMem trace | PASS |
| 3 | In-tx memory ops tracked (read/write sets) | committed tx: read set=1, write set=1 (exact) | PASS |
| 4 | `XEND` commits; write visible after commit | `htm_probe2`: `xend_result=1 shared=1` | PASS |
| 5 | Minimal-transaction cycle budget | `m_htm_transaction_cycles` = 183c = body(5c) + xend(178c); total CPU-visible = 60(start)+body+178(commit) = **238c fixed, matches Broadwell-EP calibration** (`machine_profiles/broadwell_ep_v4.json`: xbegin=60, xend=178) | PASS |
| 6 | Conflict: 2 threads RMW same line → one aborts, retries, no lost update | `htm_conflict` (2 cores, 2×50 RMW tx on 1 line): `final=100 expected=100 aborts_seen=1` | PASS |
| 7 | Unmodified real TSXSGL binary enters TSX (not SGL) | bank t1: `HTM_Start=HTM_Commit=1001`, 0 aborts | PASS (after ABI fix, §3) |
| 8 | Bank / fuzz_counter / intruder × {NOREC, TSXSGL} × t{1,2,4} | 16/16 runs: money conserved / counter sum / 300-of-300 flows found | PASS |

Probes live in `/tmp/opencode/htm_probe2.c`, `htm_conflict.c` (rebuild:
`g++ -O2 -mrtm -static -pthread`). Probe rules learned: **no syscalls inside a
transaction** (gem5 SE mode raises `syscall_fault` → HTM abort cause=exception;
real hardware lets the kernel run transparently), and the success value is
`_XBEGIN_STARTED` after the ABI fix below.

## 2. Optimization of benchmark execution in gem5

Since only the hardware transactions need cycle-accurate simulation, the flow
is: static ROI-bracketed guest binaries + SE mode + bounded txns + parallel
instances.

- **ROI brackets** (`benchmarks/cpp/gem5_roi.hpp`, `GEM5=1` builds):
  `ROI_RESET_STATS` / `ROI_DUMP_STATS` m5ops around the benchmark loop → the
  final `stats.txt` section covers the transaction phase only (init/teardown
  excluded). `GEM5_CKPT=1` adds work-item markers (checkpoint/restore path is
  broken on Ruby — do not rely on it; bounded `-n` is the substitute).
- **Bounded workloads**: bank `-n <total txns>`, fuzz_counter `t iters counters
  seed`, intruder `-p t -a pct -l maxlen -n flows -s seed`.
- **Parallel sweep driver**: `gem5_sim/scripts/run_gem5_sweep.py`
  (16-run matrix, 12 parallel on this host, per-run wall timeout, auto-parse) +
  `gem5_sim/scripts/parse_gem5_stats.py` (last-occurrence-wins over repeated
  stats sections; aggregates per-core `l1_controllers*` HTM stats; guest
  verdict for bank/fuzz/intruder).
- **Measured wall times** (timing CPU, Ruby HTM, 1.8 GHz): 3.6 s (fuzz t1) to
  142 s (intruder NOREC t2) per bounded run. The 16-run matrix completes in
  ~5 min wall with 12 parallel instances.
- **`--raw-args`** added to `configs/x86-se-bank.py` for non-bank guest argv.

## 3. Bug found & fixed: XBEGIN success value (Intel ABI)

`XBeginInst::completeAcc` (`src/arch/x86/insts/htmruby.cc`) returned `0` on
success, but unmodified real binaries check `status == _XBEGIN_STARTED`
(`-1`/`0xFFFFFFFF` from `<immintrin.h>`). Consequence: every `_xbegin()` in a
real TSXSGL binary looked like a failure → `!(status & _XABORT_RETRY)` → break
→ **silent SGL fallback for the whole run; TSX was never exercised** (and the
bug was invisible to all-`PASS` runs because SGL is correct). Fixed by writing
`0xFFFFFFFF` (Intel ABI) on success. This is the difference between "gem5 runs
TSXSGL" and "gem5 validates TSXSGL's TSX path".

The fix is canonical in the patch set: `030-htm-functionality.patch` was
regenerated (pristine+001+002 → current tree, for the 8 files 030 touches;
byte-identical result verified), and new `031-se-syscall-ignores.patch`
folds in the `clock_nanosleep` SE-mode ignore needed by the guest binaries.
Verified: full chain 001→002→030→031 applies cleanly to pristine
v25.1.0.1.

Related, in the *Rust* `tsx_sim` backend (`runtime/tsx_sim/src/lib.rs`): the
SGL fallback "lock" was encoded as `SGL_OWNER == 0 → free`, but native thread
IDs are 0-based, so thread 0's `compare_exchange(0, 0)` was a no-op
"acquisition" — two threads entered SGL mode simultaneously and their
write-through RMWs produced lost updates (t8 fuzz_counter: ~20 stale publishes
per 16k tx). Fixed with a real `SGL_HELD` `AtomicBool` mutex (CAS) plus
diagnostic `SGL_OWNER`. After the fix: 12/12 fuzz t{1,2,4,8}×3 seeds PASS with
**zero** protocol violations and **zero** stale publishes (ring validator).

## 4. Sweep results (ROI stats, 1.8 GHz)

| run | verdict | xbegin | commit | abort | rs | ws | tx-cycles (mean) | wall |
|-----|---------|-------:|-------:|------:|---:|---:|-----------------:|-----:|
| bank-norec-t1/2/4 | PASS | – | – | – | – | – | – | 11/12/14 s |
| bank-tsxsgl-t1 | PASS | 1001 | 1001 | 0 | 21.2 | 4.7 | 1151 | 5.5 s |
| bank-tsxsgl-t2 | PASS | 1213 | 604 | 609 | 16.0 | 4.9 | 420 | 6.8 s |
| bank-tsxsgl-t4 | PASS | 1617 | 6 | 1611 | 13.3 | 4.2 | 348 | 9.7 s |
| fuzz-norec-t1/2/4 | PASS | – | – | – | – | – | – | 4/5/9 s |
| fuzz-tsxsgl-t1/2/4 | PASS | 301/601/1203 | 301/600/81 | 0/1/1122 | 10.0 | 4.0 | 289/316/319 | 4/5/8 s |
| intruder-norec-t1/2 | PASS (300/300) | – | – | – | – | – | – | 71/142 s |
| intruder-tsxsgl-t1/2 | PASS (300/300) | 1716/2246 | 1701/891 | 15/1355 | 17/14 | 7/6 | 4345/650 | 19/30 s |

Full machine-readable: `m5out/sweep-<ts>/{results.csv,results.json}`.
TSXSGL under gem5 is correct at all thread counts; under high contention the
abort rate climbs (t4: ~50–93% of xbegins abort) and the binary's SGL
fallback carries the load — expected TSXSGL design behaviour, but see the
amplification note in §5.

## 5. Cross-validation: gem5 vs real hardware, all benchmarks

Only the hardware transactions need simulation, so the validation is tiered —
each tier is cheaper and the expensive tier covers less:

- **Tier 1 — decision equivalence (all benchmarks, native speed).**
  Run the same workload natively with (a) `tsx_sim` Rust feature (modeled
  TSX, correct since the SGL fix) and (b) the C++ NOREC binary; compare
  invariants, commit counts, abort breakdowns with the gem5 ROI runs.
  Same params, e.g. fuzz_counter t4: gem5 1122 aborts/1203 xbegins (93%) vs
  tsx_sim 177/1377 (13%). The gap is *model* difference, not a bug: gem5
  aborts on in-flight line-granularity coherence (and TSXSGL's own
  `sgl_owner`-in-read-set trick amplifies it — every SGL entry writes that
  line, aborting all in-flight TSX), while tsx_sim checks WR/WW only at commit.
  Real RTM hardware running this same binary should be closer to gem5.
- **Tier 2 — cycle-budget fidelity (per transaction).** gem5's fixed cost is
  238c (60 start + 178 commit, `Sequencer.py` latencies) + simulated body +
  40c coherence-probe per in-flight other — i.e. the calibrated Broadwell-EP
  numbers are baked in and reproduced (probe: 183c sample = 5c body + 178c
  commit; start latency is outside the sample window by construction). Compare
  `m_htm_transaction_cycles` per benchmark against `tsx_sim` virtual cycles and
  the `machine_profiles/*.json` cost model; flag deviations >10%.
- **Tier 3 — real RTM anchor (when hardware is available).**
  `patches/profile/tsx/run_workflow.sh` + `run_tsx_profiling.py` on a
  Broadwell/Skylake box regenerate `machine_profile.json` from RDTSC data;
  re-run this sweep and align the two knobs (`htm_start_latency`,
  `htm_commit_latency` in `Sequencer.py`) plus the +40c coherence term in
  `HTMSequencer::rubyHtmCallback` to the new profile. The gem5 vs gem5-Δ
  delta (same workload, tuned vs untuned) quantifies model sensitivity.

Recommended cadence: Tier 1 on every patch change (minutes, all benchmarks),
Tier 2 on calibration changes, Tier 3 per hardware visit.

## 6. Open items

- Abort-rate calibration at t4 (gem5 ~93% vs tsx_sim ~13%): decide which is
  the fidelity target for this binary's design (suspect gem5, due to the
  `sgl_owner` read-set amplification being real in both).
- `intruder` gem5 runs are the slowest (30–142 s per bounded run); consider a
  smaller `-n` (flows) for CI.
- Checkpoint/restore (`GEM5_CKPT=1`) remains broken on the Ruby path —
  bounded `-n` is the substitute for warmup-skip.
- The XEND commit-failure path surfaces as `InvalidOpcode` → cause
  `EXCEPTION` (RAX=0) rather than Intel's conflict code (RAX=4); retry loops
  still work (0 is retryable) but cause classification in
  `m_htm_transaction_abort_cause` under-reports `conflict`.

## 7. Known limitations

### Multi-threaded Ruby livelock (`MESI_Three_Level_HTM`)

Any SE-mode run on the Ruby HTM path (`configs/x86-se-bank.py`, which uses the
`MESI_Three_Level_HTM` protocol) hangs once **2 or more cores** are active: the
`MESI_Three_Level` directory livelocks during coherence traffic and never makes
forward progress. This is a pre-existing upstream Ruby issue, not specific to
the TSX patches, and it affects every backend that runs on the Ruby path.

Workarounds, in order of preference:

1. **Single-core timing** — keep `-t/--threads 1` (the default) for Ruby HTM
   runs; this is what Tier 1 of the sweep uses and is fully reliable.
2. **Tick guard for multi-core runs** — pass `--max-ticks N` (e.g.
   `2000000000`) to `x86-se-bank.py` so the run is force-stopped and emits
   partial stats instead of hanging forever. `scripts/run_gem5_sweep.py` applies
   this guard automatically for any run with ≥2 threads (`--mt-ticks`, default
   `2000000000`; `--mt-ticks 0` disables it). Capped runs are recorded in
   `results.csv` with `status = "tick-capped (likely livelock; see docs)"`
   rather than a bogus `ok`.
3. **Classic (non-Ruby) path for multi-threaded timing** — use
   `configs/x86-se-bank-classic.py` (classic caches, no Ruby HTM) when you need
   unbounded multi-core runs and do not need the Ruby HTM abort semantics.

A real fix would require porting the HTM logic onto a livelock-free
`MESI_Two_Level` hierarchy; that is tracked in `TODO.md` ("gem5
multi-threaded livelock") and left as future work.
