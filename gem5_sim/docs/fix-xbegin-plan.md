# Fix Plan: XBEGIN Not Called Under gem5 (TSXSGL Falls Back to SGL)

**Date:** 2026-08-22
**Status:** In progress — XBEGIN confirmed, XEND blocker found (see §6)
**Owner:** TM/gem5 integration

---

## 1. Problem Statement

`TSXSGL` (`backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp:178`) should take the
`XBEGIN`/`XEND` fast-path when executed under `gem5` (`X86_TSX` build), but
on this host (`M1/M2`, `Docker linux/amd64` → `QEMU`) it always falls back
to `global_tx_lock`/`sgl_owner` (`SGL`). Native `QEMU` not exposing `RTM`
is expected; `gem5` SE-mode with `MESI_Three_Level_HTM` *should* expose it
after `gem5_sim/patches/005-cpuid.patch`, yet observed behaviour is:

* `Docker` native: `[tm_rtm] RTM not available -- using SGL fallback` (correct,
  `QEMU` `CPUID.07H:EBX[11]==0`).
* `gem5` SE `timing`/`o3`: either no `XBEGIN` attempt (silent `SGL`) or
  `arch/x86/faults.cc:132 panic: Unrecognized/invalid instruction {0F 01 D5}`
  (`XEND`) / `arch/x86/insts/htm.cc:44 panic: XBEGIN not implemented for
  atomic memory`. No `m_htm_*` stats in `stats.txt`.

`XBEGIN` **should** be available under `gem5`; it is not being called.

---

## 2. Root-Cause Hypothesis

`TSXSGL` gates `XBEGIN` on `tm_rtm::available()` (`backends/tm_impl/common/tm_rtm.hpp:16`):

```cpp
__cpuid_count(7,0,a,b,c,d);          // b[11] == RTM
if (b & (1<<11)) {
  if (_xbegin()==_XBEGIN_STARTED) { _xend(); cached=1; } // live probe
  else cached=0;
} else cached=0;
```

Three independent gates can suppress `XBEGIN`:

| Gate | Repo state | Expected for `gem5` | Observed failure mode |
|------|------------|---------------------|-----------------------|
| **A. Guest binary built without `-mrtm`** | `benchmarks/cpp/Makefile: GEM5=1` does *not* add `-mrtm`; only `BACKEND=TSXSGL` is expected to do so implicitly via `<immintrin.h>` intrinsic lowering | `bank_gem5_tsxsgl` should contain `c7 f8 ... xbegin` / `0f 01 d5 xend` | If built with generic `clang` without `-mrtm`, intrinsic may be `ifunc` or `ud2` |
| **B. `CPUID.07H` not advertising `RTM`** | `src/arch/x86/X86ISA.py:87 ExtendedFeatures = [0x0,0x01800000,0,0]` upstream; `gem5_sim/patches/005-cpuid.patch` flips to `0x01800800` (`bit 11`) | `gem5` `X86ISA` Python param is read at simulation start, so patched source → `CPUID` reports `RTM` | Patch is in `~/Projects/SIM/TM-SIM/gem5/src/arch/x86/X86ISA.py` (verified `grep 0x01800800`), but Python-file change does not require `gem5.opt` rebuild; still requires `gem5` restart. If `gem5_sim/gem5` (gitignored clone) is unpatched, `CPUID` will still be `0`. |
| **C. Decoder / HTM model not built for this CPU/memory combo** | `src/arch/x86/isa/decoder/one_byte_opcodes.isa: C7 /3,7→XBegin` and `two_byte_opcodes.isa: 0F 01 D5/D6→XEnd/XTest` + `src/arch/x86/insts/htmruby.cc` | `XBEGIN` should decode and `XBeginInst::execute` should route to `HTMSequencer::htmBegin` for `Timing`/`O3`+`Ruby`; `Atomic` intentionally panics (`htm.cc:44`) | Installed `~/Projects/SIM/TM-SIM/gem5/build/X86_TSX/gem5.opt` (`Aug 21 23:05`) panics at `XEND` decode for `timing`/`o3` (`faults.cc:132 {0F 01 D5}`) despite patched source (`two_byte_opcodes.isa:101 XEnd`). Indicates stale `build/X86_TSX/arch/x86/X86Decoder*.o` (source `Jul 5` < binary `Aug 21` but `scons` may have missed dependency), or `MESI_Three_Level_HTM` not selected for this `build_opts/X86_TSX` in the `gem5_sim/gem5` clone. |

`GEM5_M5OPS` suppresses the `fprintf` in `tm_rtm::available()` (`#ifndef GEM5_M5OPS`), so the `SGL` fallback is silent under `gem5`.

The low-contention `100% write` workload (`bank -a 1024 -r 0`) was measured as `SGL` vs `NOrec` (`1.8×` at `1T`, `1.28×` at `4T` in `gem5_sim/docs/tsx-gem5.md:§4.3`) — correct for instrumentation overhead, but not for `HTM` lock-elision scaling (which requires the `XBEGIN` path).

---

## 3. Plan

### Phase 0 — Reproduce & Instrument (no code change, ~30 min)

1. **Prove guest binary is HTM-capable:**
   ```bash
   docker run --rm --platform linux/amd64 -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 \
     sh -c "apk add --no-cache g++ make binutils >/dev/null && make BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl && llvm-objdump-22 --disassemble bin/bank_gem5_tsxsgl | grep -c xbegin; nm bin/bank_gem5_tsxsgl | grep -i xbegin"
   # Expect ≥2 xbegin, 2 xend
   ```
2. **Probe `CPUID` under `gem5` vs native `QEMU`:**
   ```c
   // /tmp/cpuid_rtm.c — build static, run under gem5 SE classic (no Ruby needed for CPUID)
   #include <cpuid.h>
   #include <stdio.h>
   int main(){ unsigned a,b,c,d; __cpuid_count(7,0,a,b,c,d); printf("EBX=0x%08x RTM=%d\n",b,(b>>11)&1); return 0; }
   ```
   ```bash
   docker run --rm --platform linux/amd64 -v /tmp:/tmp -v "$(pwd)":/w -w /w alpine:3.20 \
     sh -c "apk add --no-cache g++ >/dev/null && g++ -static -O2 /tmp/cpuid_rtm.c -o /tmp/cpuid_rtm_static && ls -lh /tmp/cpuid_rtm_static"
   ~/Projects/SIM/TM-SIM/gem5/build/X86_TSX/gem5.opt -d /tmp/cpuid gem5_sim/configs/x86-se-bank-classic.py --binary /tmp/cpuid_rtm_static --threads 1 --accounts 16 --txns 1 --cpu-type atomic
   cat /tmp/cpuid/simout.txt   # Expect RTM=1 after patch, RTM=0 before
   # Same binary under QEMU:
   docker run --rm --platform linux/amd64 -v /tmp:/tmp alpine:3.20 /tmp/cpuid_rtm_static  # Expect RTM=0
   ```
3. **Make `available()` visible under `gem5`:**
   Temporarily build `bank_gem5_tsxsgl` *without* `-DGEM5_M5OPS` or add an explicit
   `m5` `pseudo_inst` print, or set env `TM_RTM_DEBUG=1` (new, see Phase 2).
   Run SE `timing` with `--debug-flags=X86,HTM` (or `--debug-flags=RubyHTM`) to see
   `XBeginInst::execute` vs `InvalidOpcode`.

### Phase 1 — Fix `CPUID` Advertisement ( `~5 min`)

* Verify `gem5_sim/gem5/src/arch/x86/X86ISA.py:87` is `0x01800800` in the **clone used for simulation** (`gem5_sim/gem5/`, gitignored). If not:
  ```bash
  ./gem5_sim/setup.sh --clone          # (re)clone v25.1.0.1
  git -C gem5_sim/gem5 apply gem5_sim/patches/005-cpuid.patch
  # or manually: sed -i 's/0x01800000/0x01800800/' src/arch/x86/X86ISA.py
  ```
* No `scons` rebuild needed for this file (Python param), but restart `gem5`.
* Acceptance: `/tmp/cpuid_rtm_static` under `gem5` prints `RTM=1`.

### Phase 2 — Fix Decoder / HTM Model Build (`~20 min` build)

* Ensure `GEM5=1` adds `-mrtm` for `TSXSGL` (explicit, not implicit):
  ```make
  # benchmarks/cpp/Makefile
  ifeq ($(BACKEND),TSXSGL)
    DEFS += -mrtm -DTM_BACKEND_TSXSGL
  endif
  ```
* Force `scons` to rebuild decoders:
  ```bash
  touch ~/Projects/SIM/TM-SIM/gem5/src/arch/x86/isa/decoder/*.isa
  rm -rf ~/Projects/SIM/TM-SIM/gem5/build/X86_TSX/arch/x86/X86Decoder*
  PYTHON_CONFIG=python3.12-config scons -C ~/Projects/SIM/TM-SIM/gem5 build/X86_TSX/gem5.opt -j$(nproc)
  # or from repo root:
  PYTHON_CONFIG=python3.12-config ./gem5_sim/setup.sh --build
  ```
  Verify: `grep -c XBegin build/X86_TSX/arch/x86/X86Decoder*.o` / successful `XEND` run.

* Enforce `Ruby + timing/o3` for HTM:
  * `gem5_sim/configs/x86-se-bank.py` already defaults to `timing` and
    `MESIThreeLevelHTMCacheHierarchy`; add an explicit guard:
    ```python
    assert CPU_TYPE != CPUTypes.ATOMIC, "HTM not implemented for Atomic — use timing or o3"
    ```
  * Document in `gem5_sim/docs/tsx-gem5.md §3.3` that `atomic` will always panic.

### Phase 3 — Make Fallback Observable (`~15 min` code)

* `tm_rtm.hpp:27` currently suppresses `fprintf` when `GEM5_M5OPS`. Change to:
  ```cpp
  #if defined(GEM5_M5OPS)
    // Use m5 pseudo-op or trace instead of host write(2) while in TX
  #endif
  ```
  Minimal fix: add `TM_RTM_DEBUG` env gate so `gem5` still logs outside a transaction:
  ```cpp
  const char* dbg = getenv("TM_RTM_DEBUG");
  if (dbg) fprintf(stderr, "[tm_rtm] RTM=%d cached=%d\n", (b>>11)&1, cached);
  ```
  and set `TM_RTM_DEBUG=1` in `gem5_sim/scripts/run-bank-gem5.sh` when `TRACE=1`.

* Add `gem5` `m5` annotation in `TSXSGL_runtime.cpp:real_tm_begin()`:
  ```cpp
  #ifdef GEM5_M5OPS
  if (tm_rtm::available()) m5_work_begin(0,0);
  #endif
  ```
  so `stats.txt` ROI excludes the `available()` probe.

### Phase 4 — Verify `XBEGIN` Path & Re-benchmark (`~30 min`)

1. **Single-thread `TSXSGL` under `gem5 timing`:**
   ```bash
   $GEM5_BIN -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --binary benchmarks/cpp/bin/bank_gem5_tsxsgl --threads 1 --accounts 1024 --txns 200000 --clk 1.8GHz --cpu-type timing
   cat /tmp/m5out/simout.txt  # PASS
   grep -E "m_htm|htmTrans|numCycles" /tmp/m5out/stats.txt
   # Expect: m_htm_transaction_cycles::mean >0, abort_conflict/capacity counters, no panic
   cat /tmp/m5out/simout.txt | grep -i "XBEGIN\|RTM" || echo "no host log (GEM5_M5OPS)"
   ```
2. **Re-run §4.3 workload** (`a=1024 r=0 n=200k`) for `NOREC` vs `TSXSGL` under `gem5 timing` and native `QEMU`:
   ```bash
   # native (QEMU) — both should be SGL vs STM as before
   docker run ... ./bin/bank -a 1024 -t 1 -n 200000 -r 0
   # gem5 — TSXSGL should now be HTM, not SGL
   $GEM5_BIN -d /tmp/m5out/bank-norec ... --threads 1 ...
   $GEM5_BIN -d /tmp/m5out/bank-tsxsgl ... --threads 1 ...
   grep simTicks stats.txt  # convert to cycles/txn: (simTicks/1e12)/(clk 1.8GHz) / txns
   ```
   *Success criteria:*
   * `gem5` `TSXSGL 1T a=1024 r=0` `simTicks` / `cycles/txn` significantly lower than `NOREC` (`~268` vs `~5700` predicted; see `compare_gem5_tsxsim.py`).
   * `4T a=1024 r=0` `TSXSGL` scales (or aborts stay `<1%` in `m_htm_abort_*`) while `SGL` fallback would serialize.

3. **Update `gem5_sim/docs/tsx-gem5.md §4.2`** to replace the `XEND` panic note with post-rebuild numbers, and add the `CPUID` probe output.

### Phase 5 — Prevent Regression

* Extend `gem5_sim/scripts/run-bank-gem5.sh` to `grep -q xbegin` the binary before simulation and fail fast with `ERROR: binary lacks RTM — rebuild with BACKEND=TSXSGL GEM5=1`.
* Add CI `gem5-se-smoke` job (allowed to fail until `X86_TSX` build is cached) that runs the `CPUID` probe and `bank_gem5_norec/tinystm` `timing` smoke; keep `bank_gem5_tsxsgl timing` as `experimental` until decoder fix is merged.

---

## 4. Out-of-Scope / Alternatives

* `upstream gem5 v25.1` does not ship `rtm.uca`; the in-tree `HTM` is `Ruby`-only. Porting to `FS` `KVM→O3` (`x86-tsx-fs.py`) is not needed for the SE `XBEGIN` availability fix.
* If `scons` rebuild remains flaky on `macOS` `Clang 21`, cross-build `gem5` in `Docker linux/amd64` (`scons` inside `ubuntu:22.04`) and reuse the `gem5.opt` artifact.

---

## 5. Effort Estimate

* Phases 0–1: <1h, no build
* Phase 2: 1 build (`15–30 min` on `M1`, cached)
* Phases 3–4: 1h bench + doc update
* **Total:** `~2h` wall, mostly `gem5` compile.

---

## 6. Update 2026-08-22 — XBEGIN Is Called, XEND Is the Blocker

**Re-test after rebuild `Aug 22 15:26` (`scons build/X86_TSX/gem5.opt -j10`, `RUBY=y`, `--with-ruby` removed):**

Minimal probe `htm_probe_static` (`-mrtm -static`, `xbegin`/`xend` verified via `objdump`):

```c
printf("before XBEGIN\n");
unsigned s=_xbegin(); if(s==_XBEGIN_STARTED){ printf("IN_TX before XEND\n"); _xend(); printf("XEND done\n"); }
```

```
$GEM5_BIN -d /tmp/htm_probe_out gem5_sim/configs/x86-se-bank.py --binary /tmp/htm_probe_static --threads 1 --accounts 16 --txns 1 --clk 1.8GHz --cpu-type timing
simout.txt:
  before XBEGIN
  IN_TX before XEND
panic: Unrecognized/invalid instruction {0F 01 D5}  (XEND) at tick 14136300
```

* **XBEGIN is now decoded and executed** (`htmruby.cc XBeginInst::initiateAcc/completeAcc` → `EAX=0`, `depth=1`, `m5` not panicking). `GEM5_M5OPS` still hides `tm_rtm` `fprintf`, but `TM_RTM_DEBUG=1` + `x86-se-bank.py` guard now surface it (Phase 3 committed `74dfa81`).
* **XEND (`0F 01 D5`) still decodes as `InvalidOpcode`** despite `generated/decode-method.cc.inc:9975 XEnd::xend` being present. Timing`/`o3` Ruby and classic `atomic` (`XBEGIN not implemented for atomic memory`) were both tested — same `0F 01 D5` panic, so decoder generation or `HTMCheckpoint` depth check in `htmruby.cc: XEndInst::initiateAcc` (`if (!inHtmTransactionalState()) InvalidOpcode`) is suspect, not the `CPUID` gate.

**Next fix (Phase 2b):** `rm -rf build/X86_TSX/arch/x86/generated && scons build/X86_TSX/gem5.opt` to force decoder regeneration, and audit `htmruby.cc: XEndInst::initiateAcc/completeAcc` `inHtmTransactionalState()` / `HTMCommit` for SE `TimingSimpleCPU` (currently `O3`-only in upstream `2014` patch). Until then, `bank_gem5_norec` (`software STM`) is cycle-accurate under `gem5 timing` (`§4.2`), while `bank_gem5_tsxsgl` proves `XBEGIN` availability but needs the `XEND` fix to report `m_htm_*` stats.

**Evidence already committed:** `gem5_sim/docs/tsx-gem5.md §4.3` low-contention `100% write` `SGL` vs `NOrec` (`1.8× 1T`, `1.28× 4T`) is the lower bound for HTM; `§4.2` `NOrec` `gem5` `~5.7k cycles/txn` vs `TSX` modeled `268` will be measurable once `XEND` decodes.
