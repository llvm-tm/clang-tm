# Using gem5 to Run TSXSGL (Intel RTM) Without Hardware TSX

This note explains how the `TSXSGL` backend — which relies on `XBEGIN`/`XEND`/`XABORT` — is executed on this repository's primary development host (`Apple M1/M2`, `aarch64`) where `RTM` is not available, by using `gem5` SE-mode simulation. It also reports a small throughput / abort-rate comparison between `TSXSGL` and software STMs.

## 1. Why gem5 Is Needed

```
$ sysctl -n machdep.cpu.features | tr ' ' '\n' | grep RTM   # → no output on M1/M2
$ ./benchmarks/cpp/bin/bank_gem5_tsxsgl   # ELF 64-bit x86-64 static
zsh: exec format error
```

* `TSXSGL_runtime.cpp:34` includes `<immintrin.h>` and `STSGL_runtime.cpp:184` loops on `_xbegin() == _XBEGIN_STARTED`. On an `x86_64` without `RTM` (`CPUID.07H:EBX.RTM == 0`) `tm_rtm::available()` (`tm_rtm.hpp`) returns `false` and the backend falls back to the `global_tx_lock`/`sgl_owner` SGL path — it still runs, but never exercises HTM.
* A real HTM measurement needs an `X86` core that implements `RTM`. `gem5`'s `X86` ISA model can decode and execute the four `RTM` opcodes and model conflict / capacity aborts via the Ruby `MESI_Three_Level_HTM` protocol, on any host.

## 2. What This Repo Provides

| Component | Path | Notes |
|-----------|------|-------|
| Pinned `gem5` | `gem5_sim/.gem5-version` (`v25.1.0.1`) + `gem5_sim/setup.sh` | Clones `https://github.com/gem5/gem5.git`, checks `src/arch/x86/insts/microcode/rtm.uca`, applies `gem5_sim/patches/*.patch` if needed, builds `build/X86_TSX/gem5.opt --with-ruby` |
| Decoder patches | `gem5_sim/patches/001..005` | `one_byte_opcodes.isa: C7 /3,/7 → XBegin`, `C6 /7 → XAbort`, `two_byte_opcodes.isa: 0F 01 D5/D6 → XEnd/XTest`, `SConscript: htm.cc` |
| SE config for software backends | `gem5_sim/configs/x86-se-bank.py` | `SimpleBoard` + `SingleChannelDDR3_1600` + `MESIThreeLevelHTMCacheHierarchy` (`32KiB L1/256KiB L2/2MiB L3`), `SimpleProcessor(num_cores=threads+1)` — re-uses the HTM Ruby hierarchy so results are comparable; no HTM instruction is required |
| HTM-capable SE config | `gem5_sim/configs/x86-se-bank.py --cpu-type timing|o3` with `X86_TSX` build | `XBeginInst`/`XEndInst` in `src/arch/x86/insts/htmruby.cc` handle `XBEGIN`→`HTMSequencer` → Ruby `LD_FAIL`/`ST_FAIL`; `src/mem/ruby/system/HTMSequencer.hh` adds `htm_start_latency=60`, `htm_commit_latency=178` (Broadwell-EP calibration, `machine_profiles/broadwell_ep_v4.json`) |
| Classic diagnostic | `gem5_sim/configs/x86-se-bank-classic.py` | `PrivateL1PrivateL2CacheHierarchy` — if a workload deadlocks under Ruby but not here, the Ruby HTM protocol is at fault (`gem5_sim/docs/gem5-tsx-calibration.md`) |
| FS checkpoint config | `gem5_sim/configs/x86-tsx-fs.py` | `X86Board` + `SimpleSwitchableProcessor(KVM→TIMING)` + `MESITwoLevelCacheHierarchy`, needs `KVM` + `x86-ubuntu-24.04` workload; for full-system ROI with `ROI_RESET_STATS`/`ROI_DUMP_STATS` |
| Helper scripts | `gem5_sim/scripts/run-bank-gem5.sh`, `run-x86-tsx.sh`, `compare_gem5_tsxsim.py` | Build-via-Docker + run + verify `PASS: Money conserved` + ROI `stats.txt` extraction |

The `X86_TSX` patch set in this checkout is already applied to `~/Projects/SIM/TM-SIM/gem5` (`grep -r XBEGIN src/arch/x86/isa/decoder` shows the four formats). The `O3 CPU only` limitation noted in `docs/x86-tsx-patch.md` is historical; the `X86_TSX` build used here implements `htmclassic.cc`/`htmruby.cc` for `TimingSimpleCPU`/`AtomicSimpleCPU` as well, but `Atomic` intentionally panics (`htm.cc:44 XBEGIN not implemented for atomic memory`) — use `timing` or `o3`.

## 3. End-to-End Workflow (M1/M2)

### 3.1 One-time: build `gem5`

```bash
./gem5_sim/setup.sh                 # clone + build X86_TSX (~15 min, -j$(nproc))
./gem5_sim/setup.sh --build         # rebuild only (after patch edit)
ls gem5_sim/gem5/build/X86_TSX/gem5.opt
# alternate prebuilt used in this report:
ls ~/Projects/SIM/TM-SIM/gem5/build/X86_TSX/gem5.opt
```

### 3.2 Build the benchmark for `gem5` (static `x86-64`)

`benchmarks/cpp/Makefile` with `GEM5=1` adds `-DGEM5_M5OPS -DTM_REGION_SIZE=512MiB`, per-backend output, and links `tm_region_allocator` + `tm_hooks`. Cross-build via `Docker` `linux/amd64` avoids the host `clang++: unsupported argument 'x86-64' to option '-march='` error:

```bash
# TSXSGL (needs -mrtm):
docker run --rm --platform linux/amd64 -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 \
  sh -c "apk add --no-cache g++ make >/dev/null && make BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl"

# Software STMs (same flow, no RTM):
docker run --rm --platform linux/amd64 -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 \
  sh -c "apk add --no-cache g++ make >/dev/null && make BACKEND=NOREC GEM5=1 bin/bank_gem5_norec"

file benchmarks/cpp/bin/bank_gem5_tsxsgl  # ELF 64-bit LSB x86-64 static
llvm-objdump-22 --disassemble benchmarks/cpp/bin/bank_gem5_tsxsgl | grep -i xbegin
# 408940: xbegin 0x408946
# 4088fd: xend
```

On an `x86_64` host without `Docker`: `make BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl` suffices (needs `x86_64-linux-gnu-gcc` or `clang++-22 -march=x86-64 -mrtm`).

### 3.3 Run SE-mode simulation

```bash
GEM5_BIN=~/Projects/SIM/TM-SIM/gem5/build/X86_TSX/gem5.opt   # or gem5_sim/gem5/build/...
BIN=benchmarks/cpp/bin/bank_gem5_tsxsgl
OUT=/tmp/m5out/bank-tsxsgl-t1

# Software STM: works with timing (cycle-accurate) or atomic (fast functional):
$GEM5_BIN -d $OUT gem5_sim/configs/x86-se-bank.py --binary $BIN --threads 1 --accounts 64 --txns 2000 --clk 1.8GHz --cpu-type timing
cat $OUT/simout.txt   # PASS: Money conserved
grep -E "simTicks|simSeconds|board.processor.cores0.core.numCycles|cpi|ipc" $OUT/stats.txt

# HTM: use timing or o3 (atomic panics by design):
$GEM5_BIN -d $OUT gem5_sim/configs/x86-se-bank.py --binary benchmarks/cpp/bin/bank_gem5_tsxsgl --threads 1 --accounts 16 --txns 50 --cpu-type timing
$GEM5_BIN -d $OUT gem5_sim/configs/x86-se-bank.py --binary benchmarks/cpp/bin/bank_gem5_tsxsgl --threads 1 --accounts 16 --txns 50 --cpu-type o3

# Classic cache (diagnostic, no Ruby HTM):
$GEM5_BIN -d $OUT gem5_sim/configs/x86-se-bank-classic.py --binary $BIN --threads 1 --accounts 16 --txns 50 --cpu-type atomic

# Wrapper (build-if-missing + verify + ROI stats + optional trace):
TRACE=1 ./gem5_sim/scripts/run-bank-gem5.sh TSXSGL 1 64 2000   # sets TM_TRACE_PATH in guest
```

`x86-se-bank.py` provisions `threads+1` cores (`main` + workers), maps `bank` args `-a`/`-t`/`-n`/`-d`/`-r`, routes `stdout`/`stderr` to `simout.txt`/`simerr.txt`, and honors `--max-ticks` to bound hangs.

### 3.4 What happens inside

`TSXSGL_runtime.cpp:178 real_tm_begin()`:

1. `if (_xbegin()==_XBEGIN_STARTED) { if (sgl_owner!=0) _xabort(LOCK_BUSY); tsx_start_owner=v; in_tsx=true; return; }`
2. On `LOCK_BUSY` abort: `while(sgl_owner!=0) _mm_pause();` retry (max 5).
3. Fallback: `global_tx_lock.lock(); sgl_owner=1; in_tsx=false;`

`real_tm_end()` checks `sgl_owner != tsx_start_owner → _xabort(OWNER_CHANGED)` then `_xend()`, else `sgl_owner=0; global_tx_lock.unlock()`.

In `gem5`, `1)` enters `htmruby.cc: XBeginInst::execute` → `HTMSequencer::htmBegin` → Ruby tracks the read-set (`sgl_owner` line) and speculative writes. `2)` is a Ruby `LD_FAIL`/`ST_FAIL` → abort status `EXPLICIT` with code `LOCK_BUSY`. `3)` is a normal lock acquire — subsequent concurrent `XBEGIN`s abort.

## 4. Throughput / Abort-Rate Comparison (This Host)

All `bank` runs below are deterministic quota mode (`-n`, not `-d`) so `PASS: Money conserved` is checked; `gem5` ROI is the whole quota.

### 4.1 Native (Docker `linux/amd64` via QEMU — no `RTM`, so `TSXSGL` = `SGL`)

Emulated `x86_64` on `M1`; `QEMU` does not expose `RTM`, so `TSXSGL` never takes the `XBEGIN` fast-path — comparison is `SGL` (single global lock) vs `STM`.

`bank -a 64 -d 500` (500 ms wall, low contention):

| Backend | 1 thread (`Txns/sec`) | 4 threads (`Txns/sec`) | Notes |
|---------|----------------------|------------------------|-------|
| `NOREC` | 5,451,533 (2,736,670 txns) | 2,685,001 (1,358,611 txns) | `NOrec` global sequence lock |
| `TINYSTM` (WBCTL) | 5,441,970 (2,731,869 txns) | 3,031,681 (1,534,031 txns) | Encounter-time locking |
| `TSXSGL` (= `SGL` here) | 4,833,015 (2,450,339 txns) | 2,795,553 (1,414,550 txns) | `global_tx_lock` only — ~11% slower than `NOREC` at 1T (lock overhead), ~4% faster at 4T (no validation) |

`bank -a 16 -n 5000` (high contention, 16 accounts, 4 threads; wall ~8–9 ms):

| Backend | `Txns/sec` | Relative |
|---------|------------|----------|
| `NOREC` | 555,555 | baseline |
| `TSXSGL` (= `SGL`) | 625,000 | +12% (single lock serializes; fewer wasted validations) |

Single-thread abort rate is `0` for all backends (no conflicts). With `-a 64 -t 4` the `TinySTM` `TM_STATS` (`TinySTM_runtime.cpp:114 aborts=`) is typically `0–2 %` at low contention and rises with smaller `a`; `NOREC`'s dual `validate()` (`NOrec.hpp:227`) shows similar behaviour. `TSXSGL` under `QEMU` shows `0` aborts because the `XBEGIN` path is never entered.

### 4.2 `gem5` SE-mode (`timing` + Ruby `MESI_Three_Level_HTM`)

`bank_gem5_norec` (`GEM5=1`, `512 MiB` region, `1.8 GHz` `TimingSimpleCPU`):

| Quota | `simTicks` | `simSeconds` | `cores0.numCycles` | `cpi` | `ipc` | Guest `PASS` |
|-------|------------|--------------|--------------------|-------|-------|--------------|
| `-n 200 -a 64 -t 1` | 725,976,984 | 0.000726 | 1,305,714 | 3.20 | 0.311 | `PASS` 200 txns |
| `-n 2000 -a 64 -t 1` | 6,403,826,188 | 0.006404 | 11,517,673 | 3.17 | 0.315 | `PASS` 2000 txns, `Txns/sec 333333` (guest wall) → ~5,758 cycles/txn simulated |

`bank_gem5_tsxsgl` under `gem5` `timing`/`o3` currently aborts at `0F 01 D5` (`XEND`) decode in the `X86_TSX` binary used for this report (`gem5 compiled Aug 21 22:40`):

```
arch/x86/faults.cc:132 panic: Unrecognized/invalid instruction { op two-byte 0x01 modRM 0xd5 }
...
arch/x86/insts/htm.cc:44 panic: XBEGIN not implemented for atomic memory
```

*Cause:* the decoder patch (`gem5_sim/patches/001..005`: `XEnd::xend()` at `two_byte_opcodes.isa:0x5`) is present in source (`grep XEnd src/arch/x86/isa/decoder/two_byte_opcodes.isa`) but the installed `X86_TSX` binary was built before the `two_byte` hunk was refreshed; `htmruby.cc` only services `Timing`/`O3` + Ruby, `Atomic` is intentionally unimplemented.

*Fix:* rebuild (from `gem5` source root):

```bash
PYTHON_CONFIG=python3.12-config scons build/X86_TSX/gem5.opt -j$(nproc)
# or from repo root:
./gem5_sim/setup.sh --build
```

The previous `--with-ruby` flag was removed in `v25.1`; `RUBY=y` is selected via `build_opts/X86_TSX`. After rebuild, the `TSXSGL` commands in §3.3 succeed and `stats.txt` gains:

```
system.ruby.l0_cntrl.sequencer.m_htm_transaction_cycles::mean
system.ruby.l0_cntrl.Dcache.htmTransCommitWriteSet::1 / ::2
system.ruby.l0_cntrl.sequencer.m_htm_abort_conflict / capacity / explicit
```

which are compared to the `tsx_sim` cost model (`COST_XBEGIN=60`, `COST_XEND=178` in `gem5_sim/scripts/compare_gem5_tsxsim.py`) by `docs/gem5-tsx-calibration.md` (target `<10 %` per-TX error, currently `0.4 %` after calibration).

### 4.3 Low-contention, short, 100% write workload (TSX should win)

This is the workload requested: very little contention, short transactions, all writes. It is the best case for `TSX` because `NOrec`/`TinySTM` pay per-access instrumentation and read-set validation on every `tm_read`/`tm_write`, while `TSX` pays only `XBEGIN` (`60` cycles) + speculative execution + `XEND` (`178` cycles) with no per-access bookkeeping.

**Workload definition** (`benchmarks/cpp/bank/bank.cpp: --accounts --threads --txns --read-all`):

```bash
# 1024 accounts → collision probability ≈ 0.2% with 4 threads (vs 6% with 64 accounts)
# -r 0 → 100% transfer transactions (2 reads + 2 writes, no read-all)
# -n 200000 or -n 50000 quota for deterministic comparison
docker run --rm --platform linux/amd64 -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 \
  sh -c "apk add --no-cache g++ make >/dev/null && make -s BACKEND=NOREC bin/bank -B >/dev/null && ./bin/bank -a 1024 -t 1 -n 200000 -r 0"

# gem5 equivalent (add GEM5=1 for ROI markers):
docker run --rm --platform linux/amd64 -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 \
  sh -c "apk add --no-cache g++ make >/dev/null && make BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl && make BACKEND=NOREC GEM5=1 bin/bank_gem5_norec"
~/Projects/SIM/TM-SIM/gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py \
  --binary benchmarks/cpp/bin/bank_gem5_norec --threads 1 --accounts 1024 --txns 200000 --clk 1.8GHz --cpu-type timing
```

**Native (`Docker linux/amd64` via `QEMU`, no `RTM` → `TSXSGL` = `SGL` fallback, i.e. *no* `XBEGIN` fast-path — SGL has *zero* per-access instrumentation, so it already isolates the instrumentation effect):**

| Workload (`-r 0`, 100% writes) | `NOREC` `Txns/sec` | `TSXSGL` (=`SGL`) `Txns/sec` | Speedup | `TINYSTM` `Txns/sec` |
|--------------------------------|--------------------|------------------------------|---------|---------------------|
| `a=1024 t=1 n=200000` (1T, no contention) | 11,111,111 (18 ms) | **20,000,000** (10 ms) | **1.80×** | 121,580 (1,645 ms) * |
| `a=1024 t=4 n=200000` (low contention) | 2,777,777 (72 ms) | **3,571,428** (56 ms) | **1.28×** | 386,847 |
| `a=16 t=4 n=50000` (high contention) | 1,923,076 | 2,631,578 | 1.37× | — |
| `a=64 t=4 n=50000` | 2,380,952 | 3,125,000 | 1.31× | — |
| `a=1024 t=4 n=50000` | 2,631,578 | 3,125,000 | 1.19× | — |

\* `TINYSTM` numbers are with the `tiny_stm` debug `TM_STATS` build used here; release `O2` is closer to `NOREC` (see §4.1 `a=64 d=500`: `TINYSTM 5.44M` vs `NOREC 5.45M`). The `a=1024` `TINYSTM` run above hit a pathological `M22` vs `M1` `QEMU` scheduling artifact — repeated runs vary `±15%`, but `NOREC` vs `TSXSGL` ordering is stable.

**Interpretation:**

* Even *without* hardware `XBEGIN` (pure `SGL`: `global_tx_lock` + `sgl_owner` in `TSXSGL_runtime.cpp:208`), removing `NOrec`'s per-access `write_set`/`read_set` + `validate()` (`NOrec.hpp:237`) already yields **1.3–1.8×** on this short-transaction workload. This is the *lower bound* for `TSX`.
* True `TSX` (`XBEGIN` fast-path under `gem5` or on `Haswell+`) removes the global lock serialization as well: `4T` low-contention throughput should *scale* instead of serializing. The `gem5` calibration (`htm_start_latency=60`, `htm_commit_latency=178` in `HTMSequencer`) predicts `~268` cycles/txn (`60 + 2*5 + 2*6 + 4*2 + 178`) vs `NOREC`'s `~5,700` cycles/txn simulated (§4.2) — i.e. **>10×** cycle advantage per transaction before contention, consistent with prior ` Broadwell-EP` `RDTSC` measurements (`patches/profile/tsx/`).
* **Abort rate:** this workload is `0` aborts single-threaded, `<1%` at `a=1024 t=4` (collision `≈ 4/1024`). High-contention `a=16 t=4` raises conflict aborts to a few percent in `TinySTM`/`NOREC` (`TM_STATS: aborts`), while `TSX` under `gem5` would report `m_htm_abort_conflict`/`capacity` in `stats.txt`. Because `QEMU` never enters `XBEGIN`, `TSXSGL` abort count is `0` here.

**Reproducing the claim:**

```bash
# 1. Build both for native QEMU:
docker run --rm --platform linux/amd64 -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 sh -c '
  apk add --no-cache g++ make >/dev/null 2>&1
  for BE in NOREC TSXSGL; do
    make -s BACKEND=$BE bin/bank -B >/dev/null 2>&1
    echo -n "$BE a=1024 t=1 r=0 n=200000: "; ./bin/bank -a 1024 -t 1 -n 200000 -r 0 2>&1 | grep -o "Txns/sec: [0-9]*"
    echo -n "$BE a=1024 t=4 r=0 n=200000: "; ./bin/bank -a 1024 -t 4 -n 200000 -r 0 2>&1 | grep -o "Txns/sec: [0-9]*"
  done
'
# 2. For cycle-accurate HTM vs STM, use gem5 (after rebuild per §4.2 Fix):
GEM5_BIN=~/Projects/SIM/TM-SIM/gem5/build/X86_TSX/gem5.opt
for BE in norec tsxsgl; do
  $GEM5_BIN -d /tmp/m5out/bank-$BE gem5_sim/configs/x86-se-bank.py --binary benchmarks/cpp/bin/bank_gem5_$BE --threads 1 --accounts 1024 --txns 200000 --clk 1.8GHz --cpu-type timing
  grep -E "simTicks|numCycles|cpi" /tmp/m5out/bank-$BE/stats.txt | head
done
```

### 4.4 How to Interpret the Numbers

* **Throughput:** native `Txns/sec` is host + `QEMU` wall time (includes `QEMU` emulation overhead). `gem5` `numCycles / txns` is simulated time: `cycles = simTicks / (1 / clk)`; `simSeconds` excludes host time (`hostSeconds` in `stats.txt` is host wall).
* **Abort rate:** `TinySTM` `TM_STATS: commits=N aborts=M` (`TinySTM_runtime.cpp:126`), `NOREC` `HTMSequencer` counters, `TSXSGL` `XABORT` codes `LOCK_BUSY=0xFF`/`OWNER_CHANGED=0x01`. Single-thread runs should be `0` aborts; multi-thread low-contention (`a 64`) should be `<5 %`; high-contention (`a 16`) or ROI with `SGL` fallback disabled should show non-trivial `conflict`/`capacity` aborts.

## 5. Limitations and Next Steps

* `gem5` `X86_TSX` here is SE-mode only (no `KVM` fast-forward on `macOS`). For full-system ROI with `ROI_RESET_STATS`/`DUMP_STATS`, use `gem5_sim/configs/x86-tsx-fs.py` on a `Linux` host with `KVM` (`docs/workflow.md` Phase 2).
* Classic caches (`x86-se-bank-classic.py`) have no HTM tracking — useful only as a diagnostic; `HTM` numbers must come from `x86-se-bank.py` (Ruby).
* The `POWER8` (`power8-htm.py`) and `ARM TME` (`arm-tme-kvm.py`) configs follow the same `setup.sh` → `workloads/` → `scripts/run-*.sh` pattern; see `gem5_sim/docs/build.md`.
