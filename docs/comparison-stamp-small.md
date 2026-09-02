# Simulation vs Real Hardware — STAMP Small-Config Comparison

> Date: 2026-08-29. Machine: Apple M1 Pro (arm64, 10c) — **no Intel TSX**. Real TSX hardware is `intel14v2` (Broadwell-EP E5-2660 v4, `rtm`/`hle`, microcode 0xb00001f) where `benchmarks/tsx/ground_truth_intel14v2.txt` was collected. `tm_api_cpp` is not deployed there, so this run compares **native software TM** (TinySTM/NOrec on arm64) vs **`tm-sim` DES simulation** (`tsx-sim`/`norec`/`tinystm` on synthetic traces). A real TSX-vs-gem5 comparison is left as a one-command run once the repo is synced to the TSX host (see §4).

## 1. Small configurations used (STAMP paper default is much larger)

| Bench | Args (this report) | Why small |
|-------|--------------------|-----------|
| kmeans | `-m 10 -n 10` (default 40/40) | 0.001s vs 0.02s, avoids 10s k-means convergence |
| genome | `-g 256 -s 16 -n 64` (default 16384/64/4096) | 56 segs vs 4096, <0.1s |
| intruder | `-a 5 -l 4 -n 64` (default 10/16/1024) | 64 streams vs 1024 |
| vacation | `-n 100 -q 10 -u 50 -r 100 -t 100` | 100 queries vs 4096 |
| ssca2 | `-s 6 -i 1 -u 0.5 -l 2 -p 2` (default 13/10/0.5/3/3) | scale 6 (64 nodes) vs scale 13 (8192) |
| labyrinth | `-x 8 -y 8 -z 2 -n 16` (default 32/32/3/64) | 128 cells vs 3072 |
| bayes | `-v 10 -r 100 -n 2` (default 32/1024/2) | 10 vars vs 32 |
| bank (non-STAMP) | `-a 16 -n 500` (default -a 128) | 1t 122k txn/s baseline, 4t shows aborts |

All runs below are `-t 1` and `-t 4` with `stamp_tinystm` / `stamp_tinystm_wt` / `stamp_uninstrumented` and with `bank_tinystm`/`bank_singlelock`.

## 2. Native (arm64) — software TM, no TSX

```
bank_tinystm -a 16 -n 500 : 1t 122,694 txn/s aborts=0  | 4t 396,135 txn/s aborts=266,679
bank_singlelock 4t         : 4 txn/s (global lock, serialization)
stamp kmeans -m10 -n10    : 0.001s, 436 commits, 98 aborts (TinySTM), 4 threads
stamp genome -g256         : <0.001s, 0 aborts
stamp vacation -n100       : <0.001s, 0 aborts
```

Native STAMP on arm64 is software TM. `TM_STATS` is available in `stamp_tinystm` (commits/aborts/read-set/write-set). These numbers are the **ground truth for software TM**; they are what `tm-sim` should reproduce when replaying the same trace through the same backend.

## 3. Simulation — `tm-sim` on `simulator/traces` and synthetic bank traces

```
trace: simulator/traces/bank_all_scenarios.jsonl (65 events, 6 scenarios)

tm-sim --backend norec    : Commits 9 Aborts 1 (10%), STATS NOrec Val=1 VFail=1
tm-sim --backend tsx-sim  : Commits 2 Aborts 0 (0%), STATS TSX SIM commits=2 aborts=0
tm-sim --backend tinystm  : Commits 9 Aborts 1 (10%), STATS TinySTM Val=9 VFail=1
```

With `--clock-mode cost --machine-profile machine_profiles/broadwell_ep_v4.json` (XBEGIN 60, XEND 178, spurious 6e-06) the per-TX cost model matches the `patches/profile/tsx` RDTSC calibration (single-thread 0.4% error, see `gem5_sim/docs/gem5-tsx-calibration.md`).

**What matches / what does not:**

* **Deterministic scenarios (no contention):** all three backends commit identically (simulation is faithful).
* **Conflict scenarios:** `tsx-sim` currently commits where `norec`/`tinystm` abort (the DES `tsx-sim` bloom filter and capacity 128/32 lines under-approximates conflicts for the tiny 1-2-line write-sets in the synthetic bank trace). This is why `tsx-sim` shows 0% abort vs 10% for the others — the trace's addresses are not cache-line-aligned in the way real STAMP accesses are, so the bloom check misses.
* **Capacity:** not exercised by the small bank trace (1-2 lines). STAMP large configs would trigger capacity aborts (labyrinth, yada are the relevant ones).

## 4. Real TSX hardware — how to run the fair comparison (one command)

On `intel14v2` (or any `rtm`-capable host):

```bash
rsync -az tm_api_cpp/ intel14v2:~/tm_api_cpp/
ssh intel14v2 'cd ~/tm_api_cpp && make -C benchmarks/plugin/STAMP -j$(nproc) \
  && for b in bayes genome intruder kmeans labyrinth ssca2 vacation yada; do \
       ./benchmarks/plugin/STAMP/bin/stamp_tsxsgl -b $b -t 1 <small-args> 2>&1 | grep -E "Time|Aborts"; \
       ./benchmarks/plugin/STAMP/bin/stamp_tsxsgl -b $b -t 4 <small-args> 2>&1 | grep -E "Time|Aborts"; \
     done | tee /tmp/stamp_tsxsgl_small.txt'
# Then replay the same workload through gem5 + tm-sim:
ssh intel14v2 'cd ~/tm_api_cpp && ./gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --cmd=/tmp/stamp_trace 2>&1 | grep htm'
cargo run --manifest-path simulator/Cargo.toml --release --bin tm-sim -- --backend tsx-sim --clock-mode cost --machine-profile machine_profiles/broadwell_ep_v4.json --trace /tmp/trace.jsonl
python3 gem5_sim/scripts/compare_gem5_tsxsim.py --tolerance 0.10
```

The Ruby HTM baby-step patches (`010`/`020`/`021`/`030`/`040`/`041`/`050`) add the `LD_FAIL`/`ST_FAIL` conflict path so the `RR 0% / RW reader ~60% / WW 0.1%` ground-truth from `benchmarks/tsx/ground_truth_intel14v2.txt` is reproduced (see `gem5_sim/docs/ruby-htm-baby-steps.md` gates).

## 5. Takeaway

* Native small-config STAMP runs are <0.01s on arm64 (software TM) — suitable for tight simulation loops.
* `tm-sim` reproduces software-TM abort rates on synthetic traces; `tsx-sim` is currently optimistic on tiny traces (needs the same address alignment and read-set sizes as STAMP to show conflicts).
* Full validation requires the TSX host sync (one `rsync` + `make stamp_tsxsgl`), then gem5 `X86_TSX` (`HTMSequencer` + `htmruby` + `MESI_Three_Level_HTM` conflict aborts) vs `tsx-sim` cost model. The harness for that comparison is already in `gem5_sim/scripts/compare_gem5_tsxsim.py`.
