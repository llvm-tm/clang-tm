# TODO

Central list of open work items across the repository.
Each item tags the affected area and priority (P0 = urgent, P1 = important, P2 = nice-to-have).

---

## P0 — Correctness

### TinySTM proactive_stop hang workaround — RESOLVED (2026-09-25)
- **Resolution**: All 15 `proactive_stop` sites + the `g_tm_stop_requested`
  plumbing removed from the three TinySTM designs. Diagnosis (stmbench7
  `-t 8 -w 3`): the "hang" was (1) O(N) linear write-set lookup making
  long-traversal transactions quadratic (single attempts > minutes; abort
  counters freeze), compounded by (2) unbounded pass-injected retry loops
  and (3) a deferred-free publication race (snapshot slot stored after
  begin, letting concurrent flushers free memory the new snapshot still
  referenced). Fixes: hybrid write-set index (`ws_index`, >32 entries) in
  `tinystm_common.hpp`; `-tm-max-retries` bounded retry in the plugin
  wrapper (stmbench7 uses 100); provisional snapshot publication in
  `TinySTM_runtime.cpp`; matching malloc-based `operator new` overrides
  for the existing `operator delete` ones. wbctl: 0/20 hangs (was 12/12).
  Residual `-w 3` crashes are tracked under "stmbench7 data-race corruption".


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
  verified by `tests/explicit-api/test_tinystm_clock_wrap.cpp` (drives the
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
- **Regression**: `tests/explicit-api/test_norec_ro2rw.cpp` — a conservation
  stress test where every transaction reads strictly before its first write
  (forcing RO→RW promotion mid-tx); fails if RO-phase reads are ever skipped.
  PASS on NOREC, NORECBF, TINYSTM.

---

## P1 — Fidelity

### gem5 multi-threaded livelock (review-02 S25: documented)
- **Files**: `gem5_sim/configs/x86-se-bank.py`, `gem5_sim/gem5/src/mem/ruby/protocol/`
- **Issue**: Any gem5 run with 2+ threads hangs (Ruby livelock in
  `MESI_Three_Level` coherence protocol). Affects all backends. Pre-existing.
- **Workaround**: Use `--max-ticks` or reduce thread count to 1. Documented in
  `gem5_sim/docs/x86-tsx-validation.md` ("Known limitations") and
  `docs/REQUIREMENTS.md`; `scripts/run_gem5_sweep.py` now auto-applies a tick
  guard to ≥2-thread runs (`--mt-ticks`, default 2e9) so the sweep terminates
  with `status="tick-capped"` instead of hanging. A real fix means porting the
  HTM logic to a livelock-free `MESI_Two_Level` hierarchy — **still open**.
- **Note (2026-10-05)**: reconfirmed on a fresh v25.1.0.1 X86_TSX build with
  the pristine TSXSGL runtime (bank t2 n=100 hangs; t1 n=20000 completes;
  2-line conflict probes complete). The §4 t=2/t=4 rows of
  `x86-tsx-validation.md` are tick-capped partial samples — see
  `gem5_sim/docs/x86-tsx-real-validation.md` §2.

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

### GPU benchmark stubs (@experimental)
- **Files**: `gpu/benchmarks/gpu_tpcc.cu` (line 28),
  `gpu/benchmarks/gpu_memcached.cu` (line 20),
  `gpu/benchmarks/gpu_kmeans.cu` (line 15)
- **Issue**: Three GPU benchmarks are draft skeletons with simplified
  algorithms. `gpu_kmeans` has no convergence loop; `gpu_memcached` uses
  a simplified key-value scheme; `gpu_tpcc` elides version lists.
- **Status (2026-09-13, review-02 S26)**: Downgraded to `@experimental`.
  `gpu/benchmarks/README.md` marks these as non-reference and excludes them
  from reported numbers. Reopen as P1 only if they are promoted to real
  benchmarks with full algorithms.

### GUST follow-ups after review-05 correctness fix
- **Files**: `gpu/backends/gpu_gust/include/gpu_gust_api.h`,
  `docs/proofs/GPU_GUST.cfg` / `GPU_GUST-liveness.cfg`
- **Issue**: (a) ~~The fixed protocol never reuses VBox slots; with
  `GPU_GUST_VBOX_DEPTH = 8`, addresses that accumulate > 7 committed
  versions permanently abort their writers (money-safe but throughput-starved
  under hot-key skew).~~ **RESOLVED (2026-09-25, review-06)**: between-launch
  window compaction (`gust_gpu_vbox_compact`, `GPU_GUST_VBOX_KEEP`) plus an
  overflow counter wired into `GUSTBatchExecutor::launch()`; the executor
  compacts whenever a batch overflowed. Hot-key smoke invariant (15 batches,
  ≥13 winners) passes; only boundary batches lose their winner.
  (b) The fixed `GPU_GUST.cfg` safety check completed green (532M states,
  222M distinct, 15 m 33 s on 8 workers — fine for nightly); the
  `GPU_GUST-liveness.cfg` run exceeded 15 min on this host and was never
  completed — wire both into the proofs nightly with a proper time budget.
  (c) review-05 fix verified on AMD only — re-run the four GUST benchmarks on
  the CUDA/RTX runner. (d) `gpu_gust_kernel.cuh` microbench path still not
  built by any canonical target (carries the same protocol fixes, untested).
- **Status (2026-09-25, review-06)**: (a) resolved via between-launch VBox
  compaction (`docs/CORRECTNESS_FIXES.md` §17).
- **Next step**: add `check-GPU_GUST` nightly job with a bounded
  `-traceGC`/`-workers` budget; CUDA re-verification when the NVIDIA host is
  available; build `gpu_gust_kernel.cuh` microbench path in CI.

### TSC-TM/CSMV simulator coverage (remaining from review-02 S24)
- **Files**: `explicit_api/rust/workspace/runtime/tsc_tm/`, `simulator/Cargo.toml`, `simulator/src/backend.rs`
- **Issue**: ~~Review-02 S24 added MVLog simulator coverage and expanded fidelity
  to 6 backends, but TSC-TM and CSMV cannot be simulated until Rust runtime
  crates exist.~~ **TSC-TM RESOLVED (2026-09-25, review-06)**: `runtime-tsc-tm`
  crate (TL2-derived; guard table with version-stamped locks, TSC/logical
  timestamps, monotonic `release_guard_with_version`, ROCO; ported from the
  review-05-fixed `backends/tm_impl/tsc_tm/tsc_tm.hpp`) wired into the
  simulator (`tm-sim --backend tsc-tm`, 5 new backend tests) and the `tm`
  crate (`--no-default-features --features tsc-tm`).
- **Next step**: decide whether CSMV should be simulated as a GPU/CPU variant
  (the C++ CPU variant already exists: `GPU_STM_CPU`/`CSMV` backends) before
  adding a Rust `csmv` runtime crate.

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

### stmbench7 data-race corruption under write-heavy load
- **Status 2026-09-28**: wbctl `-w 3` 0/8 clean after the deferred-free
  EBR fixes (CORRECTNESS_FIXES §18).  wbetl still 2-3/3 crashes at any
  write share (even `-w 1`); wt also crashes now (0/3 → 2/2 once real
  frees execute — the cycling bug had masked it).  New evidence: canary
  locals in `worker()` prove live frames are trampled within the first
  seconds (`ops=3`); the corruption is constant, crashes are luck.
  MALLOC_CHECK finds no heap-metadata damage and ASAN reports nothing
  (stale-pointer writes into *live* objects/frames).  Watchpoints on
  op ret-slots only caught legit prologue pushes.  Suspect narrows to
  the TinySTM TB lock-table + lazy-writeback interaction under
  same-bucket address collisions; next steps: (a) bucket-collision
  stress unit test (two addresses hashing to one `g_locks_wbetl`
  slot, interleaved commit/abort), (b) per-word lock audit counters,
  (c) `rr`/record replay once available.
- **Repro**: `stmbench_tinystm_{wbetl,wbctl,wt} -t 8 -w 3 -d 3000`
  (deterministic-ish: HEAD wbetl crashed 8/10 before this session; still
  12/12 after the quadratic/race fixes — pre-existing).
- **Signatures**: jump-to-small-int via `desc.func`/`wrap_sm*` under gdb
  (-O0 and -O2); glibc `double free or corruption (!prev)` aborts;
  underflowed category counters in final stats.
- **ASAN (link-interceptor build)**: no use-after-free/overflow, but an
  820k alloc-dealloc-mismatch storm (fixed this session via malloc-based
  `operator new` overrides — delete previously `free()`d operator-new
  blocks while `operator new` was NOT overridden).
- **Suspects**: out-of-TX `pick_*()` readers of `TMSafeVector::size()`
  vs in-TX pushes; multi-word `std::vector` members (`CompositePart::
  baseAssemblyIds`) committed word-by-word (WBETL applies eagerly, so
  readers can see half-initialized headers).
- **Next step**: full frontend-ASAN build (`-fsanitize=address` at
  compile+instrument+link) once the pass tolerates ASAN intrinsics
  (`-tm-allow-opaque` on `asan.*`), or rework SM payloads to POD.

### stmbench7 -O1 crash
- **Files**: `benchmarks/plugin/stmbench7/STMbench7.cpp`,
  `plugin/passes/TMInstrumentFnPass.cpp`
- **Issue**: `stmbench_tinystm_wbctl` crashes with null-pointer deref in
  `__tree_balance_after_insert_tm_clone` at O1. The instrumentation pass
  does not handle `invoke` instructions (only `CallInst`), so `_Znwm` calls
  inside inlined STL code are not replaced with `tm_malloc`.
- **Next step**: Add `InvokeInst` handling to the instrument pass, or use
   `-O0 -always-inline` to avoid `invoke` generation.

### `tests/plugin/regression/perf.cpp` include fix (R-18)
- **File**: `tests/plugin/regression/perf.cpp`
- **Status**: ✅ Fixed 2026-09-21. Modernized perf.cpp's missing `<mutex>` /
  `<condition_variable>` includes; it now compiles cleanly with `clang++-22`.
- **Removed**: the dead `tests/plugin/regression/old_code/` directory (3 files
  from the 2007-2012 upstream TM project, depends on a missing `Makefile.common`
  and on `stm.h` / `wrappers.h` / `mod_mem.h` that are not in this repo).

---

### Regenerate docs/book/digest.txt against the post-Ch.7 structure (P2)
- **Status 2026-10**: digest carries a staleness banner and a new Ch.7 entry
  plus corrected front-matter bullets, but its per-chapter entries still use
  old part/chapter numbers. Regenerate from `docs/book/summaries/` +
  `tools/extract_book_structure.py` when the book is touched again.

### docs/proofs full `make check` sweep is slow on heavy models (P2)
- **Status 2026-10-04**: sweep stopped early by request. DESEngine (fixed:
  saturating counters) and DUDETM (smoke cfg) now terminate quickly.
  GPU_JVSTM and possibly other GPU_*/heavy models still need
  `*-smoke.cfg` bounded configs (smaller Addr/Data/thread sets) added to
  make the full sweep finish in minutes; the mechanism is in place (check
  prefers `$b-smoke.cfg`). Also clean `docs/proofs/states/` between big
  runs (~half a TB of checkpoints observed).

## P2 — Roadmap: consistency models in the book (opened 2026-10-05)

Motivated by the EC-compensation research thread (`research/ec-bank/`):
our default teaching stance (opaque/strict TM) is one point on the
consistency/overhead curve; the book should expose the curve itself.

- [x] **Audit current coverage.** Consistency models appear only in passing
  (ch02/ch03 history, ch04/ch07/app03 anomalies). Decide: deepen Ch.7 vs.
  new chapter "Consistency & Coordination".
  (Done 2026-10-09: Ch.7 *is* the isolation-levels chapter and its cliff
  section is the exact join point, so we deepened Ch.7 with
  \S7.7 "Beyond the Cliff: When the Application Owns the Invariant"
  (label `sec:iso-beyond-cliff`) instead of renumbering the book.)
- [x] **Weaker-than-opaque section.** Argument to develop: opacity is often
  too strident for applications that can define their own invariants; if the
  app owns invariant management, the runtime can drop synchronization on
  commit (EC/compensating path, `research/ec-bank/`) — more app complexity
  for less coordination overhead. Quantify with bank numbers we already
  have (TSXSGL t1 6.67M txn/s vs t4 1.25M under contention).
  (Done 2026-10-09: \S7.7 + Table `tab:ec-vs-stm` with the TSXSGL HW
  numbers and conflict-abort mix; invariant confluence
  \cite{bailis14confluence}, SEC \cite{burgader17sec}, sagas
  \cite{garciamolina87sagas} added to references.bib.)
- [x] **Bank write-skew under SI demo.** Craft transfer transactions so the
  classic SI anomaly shows on our own toy: e.g. two transfers A→X1 and B→X2,
  each guarded by the predicate `A + B >= 16`; disjoint write-sets, shared
  predicate ⇒ SI commits both, invariant breaks; opaque backends prevent it.
  Needs an SI-like teaching runtime (extend `examples/eager_broken` or add
  `SI_STM`) + TLA+ mirror (BankSI.tla already demonstrates skew). Other
  hazards worth the same bank treatment: dirty read, non-repeatable read,
  lost update, fan-out.
  (Done 2026-10-09: `benchmarks/cpp/sidemo/bank_si.cpp` — standalone
  teaching runtime `minitm` (RU/RC/SI/opaque), five scenarios + contrasts,
  barrier-deterministic, `make -C benchmarks/cpp run-sidemo`, exits 0 iff
  exactly the 5 expected anomalies are observed. TLA+ mirrors:
  BankSI-demo-{skew,serial}.cfg (A=B=12, DRAW=8, RESERVE=16 — the model's
  predicate is "combined ≥ RESERVE *after* your draw", so 12/8/16 is the
  self-consistent triple matching the demo) and new BankWeak.tla +
  BankWeak-{ru,rc,si}.cfg for dirty read / non-repeatable read / lost
  update. All wired into `make -C docs/proofs check-teaching`. Fan-out has
  no TLA mirror (book exercise instead).)
- [x] **Compact mathematical notation throughout.** Define a small
  notation (e.g. views as folds over op-sets `view_r = fold(A_r)`,
  anomaly predicates over histories, `Inv(h)`, per-model quantifier
  skeletons) used uniformly in Ch.5/7 + TLA+/PoC artifacts, so each model
  gets a one-line definition before its prose.
  (Done 2026-10-09: front-matter "Notation: Histories, Models, and Views on
  One Page" (`cha:notation`): h/comm(h)/live(h)/⊑/<_RT, SER/SSR/OPQ/SI/EC+SEC
  skeletons, Inv(h), anomalies as history classes with the
  ∃h: L(h)∧A(h) reading; applied in Ch.2 (SER/OPQ one-liners), Ch.5 §5.4
  (h-uniform + model symbols), Ch.7 ladder bullets/witnesses/§7.7, glossary.)
- [ ] **EC follow-ups** (see research/ec-bank/README.md): liveness proof of
  compensation in TLA+, batched compensation, richer invariants via a
  confluence checker.
  - **Done 2026-10-09**: C++ EC-throughput benchmark
    (`research/ec-bank/ec_bank_bench.cpp`) added; `tab:ec-vs-stm` and §7.7
    now carry measured numbers (strict TSXSGL vs EC on the same Xeon
    E5-2660 v4: 10.5→2.7M txn/s vs 0.77→0.60M ops/s, fold cost 22×,
    compensation rate 16–24%). Remaining follow-up: model the *networked*
    case (the in-process replica sim understates EC's win by excluding
    coordination RTT).

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
- [x] MVLog simulator coverage and 6-backend fidelity sweep (fixed 2026-09-17, review-02 S24 feasible scope)
