# TODO

Central list of open work items across the repository.
Each item tags the affected area and priority (P0 = urgent, P1 = important, P2 = nice-to-have).

---

## P0 — Correctness

### TinySTM proactive_stop hang workaround
- **Files**: `backends/tm_impl/tiny_stm/tinystm_wbctl.hpp` (lines 139, 197, 339, 620),
  `tinystm_wbetl.hpp` (lines 157, 191, 254, 344),
  `tinystm_wt.hpp` (lines 200, 250, 321, 440),
  `tinystm_globals.hpp` (lines 23, 44, 65),
  `tinystm_common.hpp` (line 355)
- **Issue**: Under high contention some TinySTM backends hang. A
  `proactive_stop` call was added as a workaround (commit 0496686) to
  force-terminate stuck transactions. The underlying hang cause is unknown.
- **Fix**: Diagnose and remove all `proactive_stop` calls (15 occurrences
  across 5 files).

### TinySTM lock owner re-check
- **File**: `backends/tm_impl/tiny_stm/tinystm_common.hpp` (line 79)
- **Issue**: Inside `get_version_versioned()` the case `is_locked() &&
  get_owner() == tx_id` is flagged `// TODO: should not happen!`. This
  indicates an internal invariant assumption that deserves verification.

### TinySTM clock wrap-around
- **File**: `backends/tm_impl/tiny_stm/tinystm_common.hpp` (line 171)
- **Issue**: `// TODO: does this work with wrap around?` — the version-clock
  arithmetic is untested against a 64-bit counter overflow. Practically
  unreachable, but unverified.

### NOrec shared-structure cleanup
- **Files**: `backends/tm_impl/norec/NOrec.hpp` (line 147),
  `backends/tm_impl/norec_bf/NOrec_BF.hpp` (line 191)
- **Issue**: `tm_exit()` does not free the shared global clock, commit lock,
  or Bloom filter (if allocated). Memory leak on process shutdown.

### NOrec read-only flag semantics
- **Files**: `backends/tm_impl/norec/NOrec.hpp` (line 496),
  `backends/tm_impl/norec_bf/NOrec_BF.hpp` (line 582)
- **Issue**: When a read-only transaction performs its first write, the code
  clears `tx->read_only` but does not abort the transaction. This means the
  transaction proceeds with a stale read-set that was not tracked for
  validation. Needs investigation: should the TX abort and restart as a
  write transaction?

---

## P1 — Fidelity

### gem5 multi-threaded livelock
- **Files**: `gem5_sim/configs/x86-se-bank.py`, `gem5_sim/gem5/src/mem/ruby/protocol/`
- **Issue**: Any gem5 run with 2+ threads hangs (Ruby livelock in
  `MESI_Three_Level` coherence protocol). Affects all backends. Pre-existing.
- **Workaround**: Use `--max-ticks` or reduce thread count to 1.

### gem5 POWER8 HTM implementation (8-patch plan, in progress)
- **Plan**: `gem5_sim/docs/power8-htm-patches.md`
- **Status** (as of 2026-09-01, commit `7c6b927501`):
  - [x] Patch 1: `src/arch/power/htm.hh` + `htm.cc` — HTMCheckpoint
    save/restore (CR0[EQ]=1 on abort for the `tbegin.; beq abort_handler`
    fallback; MSR[ts] clear; TEXASR/TFIAR recording; PC → TFHAR).
  - [x] Patch 6: `src/arch/power/regs/int.hh` — TEXASR/TFIAR int regs.
  - [x] Patch 2 (partial): `src/arch/power/insts/tm.hh` — TBegin/TEnd/
    TAbort/TCheck/TSr class declarations written; **`tm.cc` not yet written**.
- **Remaining**: write `insts/tm.cc`; Patch 3 (decoder `tm.isa`/X_XO
  entries), Patch 5 (`setAbortStatus` refactor, move x86 EAX block),
  Patch 4 (`isa.cc` startup checkpoint), Patch 7 (SConscript), Patch 8
  (config + POWER8 asm bank test), then build `gem5.opt` power + smoke
  test per plan §9.
- **Notes**: tbegin. CR0 polarity (EQ=0 started / EQ=1 failed) verified vs
  Linux kernel TM docs + LLVM HTM builtins; ACHILLES `htm_test.S`/`tbegin_tend.S`
  have inverted polarity — do not mimic. SPR map: TFHAR=128, TFIAR=129,
  TEXASR=130. tcheck CR field: 0 non-tx / 1 suspended / 2 transactional.

### GPU benchmark stubs
- **Files**: `gpu/benchmarks/gpu_tpcc.cu` (line 28),
  `gpu/benchmarks/gpu_memcached.cu` (line 20),
  `gpu/benchmarks/gpu_kmeans.cu` (line 15)
- **Issue**: Three GPU benchmarks are draft skeletons with simplified
  algorithms. `gpu_kmeans` has no convergence loop; `gpu_memcached` uses
  a simplified key-value scheme; `gpu_tpcc` elides version lists.

### NOrec plugin-mode bypass (incomplete)
- **File**: `backends/tm_impl/norec/NOrec.hpp`
- **Issue**: `#ifdef LLVM_TM_PLUGIN` guards in `read_word_norec()` and
  `write_word_norec()` bypass TM tracking for addresses outside the mmap'd
  TM region. Benchmarks allocating on the regular heap get zero
  transactional protection. Partially fixed (commit path); read/write
  paths still affected.

---

## P2 — Cleanup

### stmbench7 -O1 crash
- **File**: `docs/DEBUG_TODO.md` (standalone debug document)
- **Issue**: `stmbench_tinystm_wbctl` crashes with null-pointer deref in
  `__tree_balance_after_insert_tm_clone` at O1. The instrumentation pass
  does not handle `invoke` instructions (only `CallInst`), so `_Znwm` calls
  inside inlined STL code are not replaced with `tm_malloc`.
- **Next step**: Add `InvokeInst` handling to the instrument pass, or use
  `-O0 -always-inline` to avoid `invoke` generation.

### old_code perf.c
- **File**: `tests/plugin/regression/old_code/perf.c` (line 320)
- **Issue**: Old regression test with unfinished measurement items (clock
  perturbation, WAR/WAW measurements, stm_malloc/free measurements).

---

## Completed

Items below were fixed and kept here for historical reference only.

- [x] gem5 LD_FAIL/ST_FAIL both MEMORY 8 (fixed 2026-08-30)
- [x] gem5 XABORT InvalidOpcode → GenericHtmFailureFault (fixed 2026-06-20)
- [x] gem5 capacity abort wiring (fixed 2026-06-20)
- [x] LEFTRIGHT bank multi-thread correctness (fixed 2026-06-20)
- [x] SPHT SGL fallback (fixed 2026-06-20)
- [x] TinySTM spin loops in simulation mode (fixed 2026-06-20)
- [x] ROMULUS read-validate (fixed 2026-06-15)
- [x] All 18 TLA+ backends pass safety invariants (fixed 2026-06-24)
