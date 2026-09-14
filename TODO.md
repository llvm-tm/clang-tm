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

### TinySTM lock owner re-check — RESOLVED (review-02 S03)
- **File**: `backends/tm_impl/tiny_stm/tinystm_common.hpp` (`Lock::unlock()`)
- **Resolution (2026-09-13)**: `unlock()` now counts the "unlock on a lock
  owned by a DIFFERENT transaction" case via `g_unlock_owner_mismatch`
  (atomic counter in the `tinystm` namespace) and fires a `TM_ASSERT` in
  debug builds. All call sites guard with `is_locked_by(tx->id)` first
  (commit paths) or iterate `locks_held` (abort paths), so the counter
  should stay 0; tests can assert it. The old silent-skip is unchanged
  for the benign already-released case.

### TinySTM clock wrap-around — RESOLVED (review-02 S03)
- **File**: `backends/tm_impl/tiny_stm/tinystm_common.hpp` (`inc_abort()`, `increment_clock()`, `get_clock()`)
- **Resolution (2026-09-13)**: The version clock never overflows raw:
  `increment_clock()` detects `res >= VERSION_MAX` (2^46-1), elects one
  thread via the `reset_locks_thr` CAS, zeroes the entire lock table
  (`reset_locks()`), and stores clock = 1; `get_clock()` spins while
  clock `>= VERSION_MASK`, so no reader observes an intermediate state.
  The incarnation counter in `inc_abort()` is masked to 3 bits and a wrap
  to 0 is safe (all acquires CAS the full state). The reset path is now
  verified by `tests/expli-api/test_tinystm_clock_wrap.cpp` (drives the
  clock to `VERSION_MAX - 1` and checks the reset; 11/11 checks).

### NOrec shared-structure cleanup — RESOLVED (review-02 S05)
- **Files**: `backends/tm_impl/norec/NOrec.hpp` (`exit()`),
  `backends/tm_impl/norec_bf/NOrec_BF.hpp` (`exit()`)
- **Issue (original)**: `tm_exit()` does not free the shared global clock,
  commit lock, or Bloom filter (if allocated).
- **Resolution (2026-09-13)**: Premise was a misreading. The shared globals
  (`global_lock`, `thr_counter`, `g_tm_abort_count`, and for NOrec-BF
  `g_gc_gen` + the Bloom filter `g_gc`) all have **static storage duration**
  (the filter is a fixed `std::atomic` array) — nothing is heap/mmap-allocated,
  so there is nothing to `munmap`/`free`; the OS reclaims them at exit. The
  only dynamic allocation, the per-thread `Transaction`, is already freed by
  `exit_thread()`. The 64 GB TM region mapping is deliberately OS-reclaimed
  (see `stm::tm_region_destroy`). Verified with
  `valgrind --leak-check=full ./benchmarks/cpp/bin/bank -t 2 -d 30`:
  `0 bytes in use at exit, no leaks are possible`.

### NOrec read-only → read-write promotion semantics — RESOLVED (review-02 S04)

- **Files**: `backends/tm_impl/norec/NOrec.hpp`, `backends/tm_impl/norec_bf/NOrec_BF.hpp`
- **Resolution (2026-09-13)**: Investigation showed reads are **always**
  recorded in `read_set` regardless of the `read_only` flag (the flag only
  gates the commit fast-path skip for pure read-only transactions). When the
  first write clears `read_only`, the commit path runs the full validation
  over the entire read-set, so reads made during the RO phase are validated.
  No abort-and-restart is needed.
- **Regression**: `tests/expli-api/test_norec_ro2rw.cpp` — a conservation
  stress test where every transaction reads strictly before its first write
  (forcing RO→RW promotion mid-tx); fails if RO-phase reads are ever skipped.
  PASS on NOREC, NORECBF, TINYSTM.

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
- **File**: `backends/tm_impl/norec_bf/NOrec_BF.hpp`
- **Issue**: `#ifdef LLVM_TM_PLUGIN` guards in `read_word_norec()` and
  `write_word_norec()` bypass TM tracking for addresses outside the mmap'd
  TM region. Benchmarks allocating on the regular heap get zero
  transactional protection. Partially fixed (commit path); read/write
  paths still affected.
- **Fixed 2026-09-13 (review-02 S01)**: `NOrec.hpp` was already migrated to
  the `isOnCurrentThreadStack` pattern; `NOrec_BF.hpp` read/write/commit
  paths were migrated the same way. Verified `bank_norec -a 128 -t 4`
  conserves money and `social_tm_norec -u 256 -t 4` invariant PASS.

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
- [x] NOrec plugin-mode read/write bypass (fixed 2026-09-13, review-02 S01; NOrec.hpp earlier, NOrec_BF.hpp now)
- [x] All 18 TLA+ backends pass safety invariants (fixed 2026-06-24)
