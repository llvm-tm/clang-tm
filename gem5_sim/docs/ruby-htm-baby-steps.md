# Ruby HTM Integration — Baby-Steps Plan

> Goal: add Intel TSX (RTM) to gem5's Ruby `MESI_Three_Level_HTM` protocol so that
> `benchmarks/tsx/ground_truth_intel14v2.txt` abort rates and `patches/profile/tsx`
> cycle costs are reproduced in simulation.
>
> Constraint: **do not fork gem5**. All changes live as patches under
> `gem5_sim/patches/` (applied by `gem5_sim/setup.sh`) so `gem5/` stays a plain
> `v25.1.0.1` checkout. No `gem5/.git` history is rewritten.

---

## 0. Ground rules

* **Patch-only.** No file is edited in-tree and committed. Every step adds or
  updates a numbered patch in `gem5_sim/patches/` and a test in
  `gem5_sim/tests/` or `benchmarks/tsx/`. `setup.sh --build` fails if a patch
  does not apply (CI guard).
* **Baby steps.** Each step is <150 lines, builds, and has a green test before
  the next step is started. `git diff --stat` stays reviewable.
* **Oracle.** `benchmarks/tsx/ground_truth_intel14v2.txt` (E5-2660 v4, pinned
  1/13, short TX) is the oracle for conflict semantics; `benchmarks/tsx/tsx_spurious.c`
  (0.0004% @1M) is the oracle for spurious rate. The Rust `runtime/tsx_sim`
  cost model (already calibrated, `spurious_abort_rate=6e-06`) is the second oracle
  for cycle costs.
* **Artifacts.** `gem5/src/` already contains `configs/ruby/MESI_Three_Level_HTM.py`
  and the HTM Ruby protocol stub; it does **not** contain `src/arch/x86/htm*` nor
  `src/arch/x86/insts/htmruby*` — those are added by this plan.

---

## 0. Baseline (already green)

* `gem5_sim/.gem5-version = v25.1.0.1`
* Patches `001-one-byte-decoder.patch`, `002-two-byte-decoder.patch`,
  `004-includes.patch` apply. `003/005/006` are currently red — reworked below.
* `scons build/X86/gem5.opt` builds. `scons build/X86_TSX/gem5.opt` must be made to build
  after each step.

Test 0 — **build + hello**:
```bash
gem5_sim/setup.sh --clone && gem5_sim/setup.sh --build   # uses X86_TSX once step 1 lands
./gem5/build/X86_TSX/gem5.opt --help | grep -q Ruby && echo ok
./gem5/build/X86_TSX/gem5.opt gem5_sim/configs/x86-se-bank.py --help | head
```

---

## Step 1 — Decoder & CPUID (rework 003/005)

**Why first:** without decode, `xbegin` is `InvalidOpcode`; nothing else can be tested.

**Patch:** `gem5_sim/patches/010-x86-decode-rtm.patch` (replaces 001/002/005)
  * `src/arch/x86/isa/decoder/one_byte_opcodes.isa` — `0xC7 /7` `xbegin rel32`
  * `src/arch/x86/isa/decoder/two_byte_opcodes.isa` — `0F 01` `xend`, `xabort imm8`, `xtest`
  * `src/arch/x86/X86ISA.py` — set `RTM` bit in `ExtendedFeatures[EBX bit 11]` (replaces 005)

**Test 1a — decode:**
```bash
cat > /tmp/xbegin.s <<'EOF'
xbegin 1f; xend; 1: xor %eax,%eax
EOF
x86_64-linux-gnu-as -o /tmp/xbegin.o /tmp/xbegin.s && llvm-objdump -d /tmp/xbegin.o | grep -q xbegin && echo ok
./gem5/build/X86_TSX/gem5.opt --debug-flags=Decode gem5_sim/configs/x86-se-bank.py --cmd=/tmp/xbegin.elf 2>&1 | grep -q XBEGIN && echo ok
```

**Test 1b — CPUID:**
```bash
./gem5/build/X86_TSX/gem5.opt gem5_sim/configs/x86-se-bank.py --cmd=/tmp/cpuid_rtm 2>&1 | grep -q rtm && echo ok
# /tmp/cpuid_rtm is a tiny ELF that does cpuid 7:ebx & (1<<11)
```

---

## Step 2 — HTM sequencer state (rework 003 SConscript + new HTMSequencer)

**Files:**
  * `gem5_sim/patches/020-htm-sequencer-params.patch` — `src/mem/ruby/system/Sequencer.py`:
    ```python
    class RubyHTMSequencer(RubySequencer):
        htm_start_latency = Param.Cycles(60, "XBEGIN calibrated")
        htm_commit_latency = Param.Cycles(178, "XEND calibrated")
        htm_capacity_read_lines = Param.Int(512, "")
        htm_capacity_write_lines = Param.Int(128, "")
        htm_spurious_rate = Param.Float(6e-06, "from ground_truth")
    ```
  * `gem5_sim/patches/021-htm-sequencer-state.patch` — new `src/mem/ruby/system/HTMSequencer.{hh,cc}`:
    `struct HtmState { int depth; AddrSet readSet, writeSet; Tick startTick; }` per sequencer,
    `isInTransaction()`, `htmBegin()`, `htmCommit()`, `htmAbort(cause)`, bloom helpers.
    Initially **no Ruby interaction** — just tracks sets and enforces capacity (abort if
    `size() > htm_capacity_*`), plus spurious `drand48() < htm_spurious_rate` at begin.

**SConscript:** `src/mem/ruby/SConscript` adds `HTMSequencer.cc`.

**Test 2 — single-thread, no Ruby conflicts:**
```bash
# uses existing benchmark, now with sequencer latencies visible in stats
./gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --cmd=/tmp/bank --num-cpus=1 2>&1 | grep -q "Final total"
grep -q "htmTxStarted" /tmp/m5out/stats.txt && echo ok
# spurious: run benchmarks/tsx/tsx_spurious compiled for gem5 SE; expect ~0.0006%
```

---

## Step 3 — x86 HTM instructions (Ruby path)

**Patch:** `gem5_sim/patches/030-htm-insts-ruby.patch`
  * new `src/arch/x86/insts/htm.hh`, `src/arch/x86/insts/htm.cc` — `XBegin`, `XEnd`, `XAbort`, `XTest` static insts
  * new `src/arch/x86/insts/htmruby.cc` — `initiateAcc()`/`completeAcc()` for Ruby:
    `XBegin: sequencer->htmBegin(fallbackRip, htm_start_latency)`; on abort: set `EAX=cause`, `RIP=fallback`
    `XEnd: if !isInTransaction() return NoFault` (SE-mode nop, replaces 006), else `htmCommit(htm_commit_latency)`
    `XAbort: htmAbort(EXPLICIT | code<<24)`
    `XTest: EAX = isInTransaction() ? 1 : 0`
  * `src/arch/x86/SConscript` — add `insts/htm*.cc`

**Test 3 — single-thread commit/abort:**
```bash
# RR-like single thread must commit; explicit abort must land on fallback
./gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --cmd=/tmp/tsx_spurious_gem5 2>&1 | grep -q "spurious abort rate"
# also: run benchmarks/tsx/tsx_spurious.c cross-compiled for gem5 SE, compare vs native 0.0004%
```

---

## Step 4 — Ruby conflict detection (the core of this plan)

### 4a — Protocol bookkeeping

**Patch:** `gem5_sim/patches/040-htm-protocol-sets.patch`
  * `src/mem/ruby/protocol/MESI_Three_Level_HTM*.sm` (SLICC): add per-line `transReadMask`, `transWriteMask` (or borrow the existing `HTM` state bits). On `L0Cache` `Load`/`Store` while `sequencer->isInTransaction()`, insert `addr>>6` into `htmReadSet`/`htmWriteSet` (the sequencer, not the protocol, owns the sets — protocol just forwards the callback).
  * New messages `HTMAbort` / callback `htmConflict(addr)` that the sequencer exposes to the protocol.

### 4b — LD_FAIL / ST_FAIL wiring

**Patch:** `gem5_sim/patches/041-htm-conflict-abort.patch`
  * `L0Cache` on `Load`/`Store` that hits a line with `transMask` of another core: send `HTMAbort` to that sequencer, which does `htmAbort(CONFLICT)` (sets EAX bit 2, jumps to fallback). This is the `LD_FAIL`/`ST_FAIL` path.
  * Important: **no handshake inside TX** — the protocol's abort is driven purely by coherence invalidations, matching the ground-truth observation that `RW` aborts the reader (writer wins) and `RR` does not abort.

**Tests 4 — the oracle:**
```bash
# cross-compile the ground-truth probes for gem5 SE (freestanding, no pthread barrier)
x86_64-linux-gnu-gcc -O2 -mrtm -static -o /tmp/tsx_rr_gem5 benchmarks/tsx/tsx_conflict_matrix_gem5.c -D RR
./gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --cmd=/tmp/tsx_rr_gem5 2>&1 | grep -q "RR.*0% abort"
# repeat for RW/WR/WW — expected from ground_truth_intel14v2.txt:
# RR 0%, RW reader 60%/writer 0.1%, WR mirrored, WW ~0.1% (free-running 1-access)
# For deterministic overlap, add a gem5-specific probe with an in-TX delay loop
# (1000 pauses) — RW reader should then abort ~100% when overlap is forced.
```

---

## Step 5 — SGL fallback & TSXSGL integration

**Patch:** `gem5_sim/patches/050-sgl-owner.patch`
  * `sgl_owner` is just a normal cache line (already in `benchmarks/tsx` probes as `data_line`). No special handling needed if step 4 is correct: a thread that does `global_tx_lock.lock(); sgl_owner=tid` while another is in TX will naturally abort the TX via the `ST_FAIL` path. The patch only adds a latency annotation (`mutex_lock_cycles=75`) and a gem5 stat `htmSglFallbacks`.
  * `XEnd` outside TX stays a nop (step 3) so the TSXSGL fallback's `xend` on the non-transactional path does not `#GP`.

**Test 5 — bank:**
```bash
./gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --cmd=/tmp/bank --num-cpus=4 2>&1 | grep -q "PASS"
# compare abort mix vs tsx_sim: tsx_sim ground-truth run is in ground_truth_intel14v2.txt
# gem5 stats: htmTxCommit vs htmTxAbort{Conflict,Capacity,Explicit,Other}
```

---

## Step 6 — Calibration & CI

**Files:**
  * `simulator/machine_profiles/broadwell_ep_v4.json` already carries `spurious_abort_rate=6e-06` and `htm_start/commit_latency`.
  * `gem5_sim/scripts/compare_gem5_tsxsim.py` — parse `m5out/stats.txt` vs `tsx_sim` `STATS` line, report per-TX cycle error (target <10%, current single-thread 0.4%).

**Test 6 — full validation (the baby-step gate):**
```bash
make -C benchmarks/tsx && scp benchmarks/tsx/* intel14v2:/tmp/ && ssh intel14v2 'cd /tmp && ./tsx_conflict_matrix RW && ./tsx_spurious 1000000 0'
./gem5/build/X86_TSX/gem5.opt -d /tmp/m5out gem5_sim/configs/x86-se-bank.py --cmd=/tmp/bank --num-cpus=1 --htm-start-latency=60 --htm-commit-latency=178 2>&1 | grep htmTx
cargo test -p tm-des --test tsx_ground_truth -- --test-threads=1  # tsx_sim oracle
python3 gem5_sim/scripts/compare_gem5_tsxsim.py --tolerance 0.10 && echo "CALIBRATED"
```

CI (`.github/workflows/ci.yml`): add job `gem5-patches-apply` that does `gem5_sim/setup.sh --clone && for p in gem5_sim/patches/*.patch; do git -C gem5_sim/gem5 apply --check $p; done` — no build, just apply-check, so every PR is verified without a full gem5 compile.

---

## File map

```
gem5_sim/patches/
  010-x86-decode-rtm.patch          # step 1
  020-htm-sequencer-params.patch    # step 2
  021-htm-sequencer-state.patch     # step 2 (new .hh/.cc)
  030-htm-insts-ruby.patch          # step 3 (new insts/htmruby.cc)
  040-htm-protocol-sets.patch       # step 4a
  041-htm-conflict-abort.patch      # step 4b
  050-sgl-owner.patch               # step 5

benchmarks/tsx/
  tsx_conflict_matrix.c             # oracle (already added)
  tsx_spurious.c                    # oracle (already added)
  tsx_conflict_matrix_gem5.c        # freestanding variant for gem5 SE (step 4 test)
  ground_truth_intel14v2.txt        # oracle data (already added)

gem5_sim/tests/
  test_step1_decode.sh
  test_step2_spurious.sh
  test_step4_conflict.sh
  test_step5_bank.sh

simulator/tests/tsx_ground_truth.rs # tsx_sim oracle (already added)
```

Each patch is ~50-120 lines; each test is a one-liner `grep` on `stats.txt` or program output. No patch touches `gem5/` history — `setup.sh` clones stock `v25.1.0.1` and applies them. To update gem5, bump `gem5_sim/.gem5-version` and rebase patches (usually trivial).

---

## What is intentionally out of scope

* Power/ARM HTM (separate protocols).
* HLE (prefix) handling — RTM only.
* Persistent HTM (NV-HTM) — separate patch set.
* Detailed L1 associativity capacity model — approximated by `htm_capacity_*` params.
