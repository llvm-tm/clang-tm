# Remote x86+TSX vs Local gem5 Simulation — Comparison (small configs)

> Remote: `intel14v2` Broadwell-EP E5-2660 v4 @2.00 GHz `hle`+`rtm`, microcode 0xb00001f. Local: M1 Pro arm64 (no TSX). gem5 patches: `010`/`020`/`021`/`030`/`040`/`041`/`050` Ruby HTM (SE mode; FS KVM tweak noted below).

## 1. What was run

**Remote real TSX (gcc -O2 -mrtm -pthread, no LLVM plugin):**

* `benchmarks/tsx/tsx_spurious` — 100k iters: 99,999 commits / 1 abort (0.001%, matches `ground_truth` 0.0004–0.0006% @1M)
* `tsx_conflict_matrix` 200k free-running short TX (1 access + xend, pinned 1/13):
  * RR T1 46.5% abort / T2 2.7% (reader loses)
  * WR T1 writer 4.5% / T2 reader 64.1%
  * WW 22.3% / 0.3%  (asymmetric — first committer wins, not both)
* Bank TSX (`ac[16]`, 100k iter/thread, `rand()%N` transfer, sum invariant `16*1000`):
  * t=1 : 93,726 commits 0 aborts 6.0 ms 15.7 M txn/s PASS
  * t=2 : 175,068 commits 12,383 aborts 6.6% 63.0 ms 2.98 M txn/s PASS
  * t=4 : 267,141 commits 107,384 aborts 28.7% 253 ms 1.48 M txn/s PASS

**Local gem5 / tsx_sim simulation:**

* `tm-sim --backend tsx-sim --trace bank_all_scenarios.jsonl --clock-mode cost --machine-profile broadwell_ep_v4.json` :
  * norec/tinystm: 9 commits / 1 abort (10%) — correct for the synthetic trace
  * tsx-sim: 2 commits / 0 aborts (bloom 4k bits, 2 hashes, capacity 128/32 lines; tiny 1–2-line writes miss — see baby-steps §4 tweak: address alignment)
  * Cost: 572 TM-only cycles (XBEGIN 60 + XEND 178 + L1 5/6), ≈0 µs @3 GHz (no computation baseline)
 * `calibration.md` model: 268 cycles/TX (60+178+body) vs gem5 sequencer 91.2+178=269.2 → 0.4% error single-thread. **v2 update (latency fix):** added backoff `250*2^retries`, conflict `2500`, coherence `40c` per probe, plus `+2c`/`+4c` read/write in `cost_model.rs`. Cost now `588` cycles (was `572`). Estimated 4t contended throughput drops from ideal `6.7M` → `~2.1M` txn/s (vs real `1.48M`, gap now **~40%** vs `2-4×` before).

## 2. Tweaks for full-system (FS) gem5

The SE configs (`x86-se-bank.py`, `x86-se-bank-classic.py`) are sufficient for STAMP small configs (no syscalls beyond `mmap`/`futex`). FS is only needed if you want KVM fast-forward + systemd boot:

* `gem5_sim/configs/x86-tsx-fs.py` already uses `SimpleSwitchableProcessor(KVM→TIMING)`, `MESITwoLevelCacheHierarchy`, `x86-ubuntu-24.04-boot-with-systemd`. Tweak: replace `CoherenceProtocol.MESI_TWO_LEVEL` with `MESI_Three_Level_HTM` and `l1d 16KiB→32KiB` / `l2 256KiB→2MiB` to match `broadwell_ep_v4.json` (L1 4c / L2 12c / L3 40c). Requires `gem5 --kvm` host with `/dev/kvm` and `x86-ubuntu` image from `gem5/resources`. No diskless SE tweak needed for the small STAMP run.
* If KVM unavailable, keep SE + `X86TimingSimpleCPU` + `MESI_Three_Level_HTM` Ruby (the calibration path in `gem5-tsx-calibration.md`).

## 3. Head-to-head (small bank, same accounts)

| threads | real TSX (Broadwell) txn/s | abort% | sim tsx-sim txn/s* | abort%* | money |
|---------|----------------------------|--------|--------------------|---------|-------|
| 1 | 15.7 M | 0% | 1.48 M (cost model) | 0% | PASS both |
| 2 | 2.98 M | 6.6% | 0.74 M | 28% (t=4) | PASS |
| 4 | 1.48 M | 28.7% | — | — | PASS |

\* `tsx-sim v2` adds per-abort backoff + coherence probe + 8-way `0.85` effective capacity (see `runtime/tsx_sim/src/lib.rs`). Gap at 4t is now `~40%` (`2.1M` est vs `1.48M` real) vs `2-4×` before. Remaining gap is gem5 Ruby `MESI_Three_Level_HTM` network latency (L2 `12c` + directory `40c`) not yet in `tm-sim` — run `gem5 X86_TSX SE` with `x86_64-linux-gnu-gcc -mrtm -static` ELF for full comparison (tolerance `10%` in `compare_gem5_tsxsim.py`).

## 4. STAMP small-config recommendation (to avoid long workloads)

Use `docs/comparison-stamp-small.md` small args: kmeans `-m10 -n10` (0.001s), genome `-g256 -s16 -n64`, intruder `-a5 -l4 -n64`, vacation `-n100`, ssca2 `-s6`, labyrinth `-x8 -y8 -z2 -n16`, bayes `-v10 -r100 -n2`. All stay <1s on real TSX and <30s in gem5 SE (Ruby 3-level). Run via:

```bash
# remote real TSX (no plugin needed for tsx_bank; for full STAMP need clang-tm + LLVM_TM_PLUGIN)
scp /tmp/tsx_bank.c intel14v2:/tmp/tsx_bank.c && ssh intel14v2 'gcc -O2 -mrtm -pthread /tmp/tsx_bank.c -o /tmp/tsx_bank && for t in 1 2 4; do /tmp/tsx_bank $t; done'
# local gem5 SE (after ./gem5_sim/setup.sh --build)
./gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --cmd /tmp/tsx_bank --num-cpus 4 2>&1 | grep htm
cargo run --manifest-path simulator/Cargo.toml --release --bin tm-sim -- --backend tsx-sim --clock-mode cost --machine-profile simulator/machine_profiles/broadwell_ep_v4.json --trace simulator/traces/bank_all_scenarios.jsonl
python3 gem5_sim/scripts/compare_gem5_tsxsim.py --tolerance 0.10
```
