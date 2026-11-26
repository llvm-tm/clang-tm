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

### CSMV/GPU_STM_CPU fixed-size read/write sets dropped overflow entries — RESOLVED (2026-11-25)

- **Symptom as filed**: `test_tx` "fails 1/120" with labels `tm_realloc`,
  `TM<int*> ptr`, `TM<TM<int>*> []`, `thread lifecycle` — but only **one**
  check actually fails: `tm_realloc tail writable`
  (`read_at(&q[n_new-1]) == n_new-1` after a 16→64 grow inside a tx). The
  other labels are the file-global `failures` counter bleeding into later
  sections (`printf("  %s: %s\n", name, failures ? "FAIL" : "PASS")` prints
  the *cumulative* count). Wave D of `bin/drv_diff_ops -M 128` reproduced it
  64/64 on CSMV and GPU_STM_CPU.
- **Root cause** (not the allocator, which is shared and fine): the CPU
  runtimes kept `reads[CSMV_MAX_READS]`/`writes[CSMV_MAX_WRITES]` = 64/32
  fixed arrays guarded by `if (num < MAX)`, i.e. **overflow entries were
  silently discarded**. The realloc test writes 16+48 = 64 words in one
  transaction, so every write past the 32nd — including the tail element it
  then reads back — was dropped on commit. A dropped read-set entry is
  equally bad: that object stops being validated.
- **Fix**: the sets grow (vectors, with the old limits kept as the initial
  reserve) in `backends/tm_impl/csmv/{include/csmv_api.h,cpu/csmv_cpu_runtime.cpp}`
  and `backends/tm_impl/gpu_stm/cpu/gpu_stm_cpu_runtime.cpp`.
- **Verification**: CSMV `test_tx` 120/120 (was 119/120) + `test_ds`
  207/207; GPU_STM_CPU 120/120 + 207/207 and `drv_diff_ops`
  `alloc_viol=0` (was 64/64). Instrumented the pre-fix code to confirm the
  mechanism: the 64-write transaction hit the drop path, while the driver's
  small transactions never did (max 4 reads / 4 writes) — which is why the
  remaining CSMV failure below is a different bug.
- **Note**: `backends/tm_impl/gpu_stm/cpu/pr_stm_cpu.cpp` (the GPU-simulation
  library, not linked into the `GPU_STM_CPU` backend) still has the same
  fixed-array pattern with silent drop at its own 64/32 limits — fix it if
  that path ever becomes load-bearing.

### CSMV version store aliased word-sharing objects — RESOLVED (found & fixed 2026-11-25)

- **Symptom (as filed)**: purely **sequential**, no contention, zero aborts.
  `bin/repro_bucket_alias BACKEND=CSMV`: 16 int32 objects in one block, each
  RMW'd +1 in its own transaction → 40/48 transactional reads wrong and
  16/16 committed values wrong (e.g. `c[0]` reads 1002 when its own history
  says 1001 — the value of its word-mate `c[1]`). `bin/drv_diff_ops`
  reported `quiesce_viol≈78-80` of 144 objects at `-T 1` too, plus
  `v_range≈200`. TL2/SGL/… clean.
- **Root cause**: `csmv_get_entry()` bucketed by `(uintptr_t)addr >> 3`,
  which erases bit 2 and so maps the two int32 halves of an 8-byte word (and
  the many sub-byte objects inside 8 bytes) to ONE entry, and `CSMVVersionNode`
  is `{timestamp, value, next}` with no address field, so those objects
  shared one version chain. `read_common()`'s fast path returned
  `entry->head->value` — whichever word-mate wrote last — and the driver's
  read-modify-write then committed that foreign value.
- **Fix**: the table is now open-addressed and keyed by the exact address.
  `CSMVObjectEntry` gained `std::atomic<void *> owner` (nullptr = unused);
  `csmv_get_entry()` hashes the full address (multiplicative), then linearly
  probes, matching `owner==addr` or claiming an empty slot via CAS; a bounded
  probe (1024) aborts loudly instead of silently sharing. Entries are claimed
  once and never released (region addresses reused by the allocator map back
  to the same entry), so there are no tombstones and no rehash. The commit
  lock-order (by entry pointer) and the read-set validation are unchanged.
  Files: `backends/tm_impl/csmv/include/csmv_api.h`,
  `backends/tm_impl/csmv/cpu/csmv_cpu_runtime.cpp`. Documented in
  `docs/CORRECTNESS_FIXES.md` §25.
- **Verification**: `repro_bucket_alias` clean; `drv_diff_ops` clean at
  `-T 1` for **every** op-class mask (M=1/2/4/8/16) and on seeds 1-3, and
  clean at `-T 4`/`-T 8` on the full mask; `test_tx` 120/120, `test_ds`
  207/207.
- **Caveat**: `csmv_api.h` comments say the validation convention is shared
  with a GPU executor. No GPU sources exist in this repo, but re-check that
  executor before relying on the new `CSMVObjectEntry` layout.

### CSMV rare lost-update / phantom under contention — RESOLVED (found & fixed 2026-11-25; self-introduced regression)

- **Symptom**: after the bucket-aliasing fix, a **rare** anomaly survived
  under real concurrency: `drv_diff_ops BACKEND=CSMV` reported `quiesce_viol`
  1–4 on ~1 in 3 `check-diff` runs (rate rose with `-n`), deltas off by 1–5 in
  **both** directions, `quiesce_viol` the only nonzero counter, `-T 1` always
  clean.
- **Actual root cause — a bug in my own §25 fix** (not pre-existing; the
  earlier "isolated / cannot be the addressing change" note here was wrong).
  `csmv_get_entry()` claimed an empty slot with
  `compare_exchange_strong`, and on CAS **failure** it unconditionally probed
  to the next slot. But the CAS usually fails because another thread just
  claimed **that same slot for the same address** (two threads first-touching
  a hot object). The loser then claimed a *second* slot for that address, so
  the address had two version chains; reads and writes landing on different
  chains lost or phantomed deltas. This is why it was CSMV-only, rare (needs
  a concurrent first-touch), `-T 1`-clean (no race), and rose with thread
  count and `-n`.
- **Fix**: on CAS failure, `expected` now holds the winning owner — if it is
  our own address, return that slot (the other thread established our entry);
  only probe on when a *different* address won it. The table is append-only
  (entries never released) and the probe order is fixed per address, so every
  lookup converges on the single slot where the address was first claimed.
  Also hardened the lockless version-chain walk: `entry->head.load` went from
  `memory_order_consume` to `acquire` (mainstream compilers compile consume as
  *relaxed*, which leaves the `head->timestamp`/`->value`/`->next` reads below
  unordered against the load — the readers do not hold the entry lock, so the
  walk depends on this acquire pairing with the committer's release-store).
- **Verification**: previously-failing configs run clean across many seeds —
  full mask `-T 4 -n 1500` and `-T 8 -n 1200` (seeds 1–8), and the
  highest-frequency `-M 1 -T 4 -n 2500` and `-M 1 -T 8 -n 3000` (seeds 1–8);
  `test_tx` 120/120, `test_ds` 207/207; `make -C tests/backends/tm_impl
  run-csmv` all PASS. Documented as `docs/CORRECTNESS_FIXES.md` §26.
- **Harness**: the `tools/diff_backends.sh` known-open allowance for CSMV was
  **removed** — CSMV is now gated on every counter like all backends.

### CSMV raw object reads were over-wide at every access width — RESOLVED (found & fixed 2026-11-26)

- **Files**: `backends/tm_impl/csmv/cpu/csmv_cpu_runtime.cpp`
- **Found by**: `-fsanitize=thread` on `tests/concurrency/drv_diff_ops.cpp`
  (6 of the 16 CSMV races were in the TM data region).
- **Issue**: the "no versions yet" fallback in `read_common()` did
  `*(uint64_t *)data_addr` regardless of access width. Reached from
  `real_tm_read_i4`/`i2`/`i1` that reads 8 bytes for a narrow object, so it
  touched the **neighbouring** object's bytes — where that object's commit
  write-back publishes its value — with a plain load against the write-back's
  atomic store, and could over-read past the end of the mapped region.
- **Fix**: `raw_load(addr, bytes)` — width-exact `__atomic_load_n` using the
  same widths as `real_tm_end`'s `__atomic_store_n` write-back; the seven read
  stubs pass their width.
- **Verification**: CSMV TSan warnings 16 → 0; driver counters all 0;
  `test_tx`/`test_ds` 120/207. `docs/CORRECTNESS_FIXES.md` §27.

### CSMV version GC freed the version a live snapshot still needed — RESOLVED (found & fixed 2026-11-26)

- **Files**: `backends/tm_impl/csmv/cpu/csmv_cpu_runtime.cpp`,
  `tests/backends/tm_impl/test_gc_reader.cpp` (new)
- **Issue**: two compounding defects. (1) The GC low-water mark was the guess
  `clock - 1000` rather than the oldest snapshot among running transactions.
  (2) The sweep unlinked every version with `timestamp < lwm`, including the
  **floor** version (newest at or below the mark) that a reader at the mark
  selects, and `gc_flush()` `std::free`d it immediately — no epoch, no hazard
  pointer. A live reader then followed a freed link (UAF) or fell off the chain
  end and read `0` where a committed value lived.
- **Fix**: active-transaction registry (per-thread slot publishing a *lower
  bound* on the snapshot before the clock is read; mark = min(clock-before-scan,
  slots)) → always ≤ every live/future snapshot; nesting-safe; stale slots
  overwritten on reuse; slots exhausted ⇒ reclamation disabled. Sweep keeps the
  floor and frees only strictly older versions, skips entries whose versions
  are all newer than the mark, and touches chain links with acquire/release
  because readers traverse them locklessly.
- **Verification**: new `gc_reader` test fails **6/6** before the fix
  (`reader saw 1 then 0`) and passes after, in a run that still reclaimed
  48393 version nodes (not a vacuous pass); `bank -t 8 -n 50000` reclaimed
  77556 nodes, money conserved; driver `-T 8 -n 3000` seeds 1–3 clean; TSan 0
  warnings; all 11 unit-test backends PASS. `docs/CORRECTNESS_FIXES.md` §28.

### Hook-table publication raced against every thread start (ALL backends) — RESOLVED (found & fixed 2026-11-26)

- **Files**: `backends/tm_impl/common/tm_hooks.cpp`
- **Issue**: `tm_hook_init_thread()` re-installed all 23 hook function pointers
  on every thread start. Instrumented code reads those globals on **every TM
  call** without `s_hook_mutex`, so the redundant re-write is a plain data race
  and a thread could in principle observe a half-installed table mixing real
  and stub hooks (stubs silently no-op transactions — the hazard that file's
  own comment warns about). TSan reported the same 10 races for every backend.
- **Fix**: publish only when the *intended* table changed (`s_hooks_dirty` /
  `s_hooks_applied` generation counters bumped by `tm_register_real_hooks()`
  and `tm_swap_runtime()`); thread-count changes no longer re-publish. Trace
  mode keeps its pending re-publish so the wrapper chain is preserved.
- **Verification**: hook-global races gone in every TSan-linkable backend;
  SGL/JVSTM/CSMV reach 0 warnings; `check-fast`, `make -C plugin run`,
  all-backends unit suite and `test_tx`/`test_ds` unchanged.
  `docs/CORRECTNESS_FIXES.md` §29.

### Follow-ups opened 2026-11-26 (P1)

- **CSMV GPU kernel still has three known gaps** (`gpu/backends/csmv/cuda/`):
  (a) no commit-time read-set validation in `csmv_device_commit` at all →
  conflicting transactions both install versions (lost updates by design);
  (b) `csmv_gpu_entry_idx()` still keys version chains by `>> 3` with no owner
  tag ⇒ the §25 sub-word aliasing, unfixed on GPU; (c) no block-0 / server-warp
  commit as specified by `docs/proofs/GPU_STM_PLAN.md:19` ("client-server
  commit: server validates, clients batch writes") — neither GPU path has a
  server. A CPU model of the server commit is in place and driver-proven
  (`CSMV_SERVER_COMMIT=1`, commit `f142ad9`); porting it to the kernel needs a
  GPU runner, because the `gpu/` tree is only *compiled* in `nightly.yml` and
  never correctness-gated.
 - **TSan sweep follow-ups (final sweep 2026-11-26)**: `-fsanitize=thread` links
   for **all** backends (the runtime `operator new/delete` overrides are gated off
   under `__SANITIZE_THREAD__`, which is what made TINYSTM/WT/WBETL/NOREC/
   NORECBF/MVLOG fail to link), `drv_diff_ops -S 1 -T 4 -n 200` reports **zero
   consistency violations for all 19 backends**, and every backend's TSan warning
   count is now **0** (ACCELTL2's on its gated CPU path — see below) — down from
   178 warnings tree-wide at the start of the night, with docs/CORRECTNESS_FIXES.md
   §29–§32 recording the causes. Warnings per backend, before → after: TL2 14→0,
   ACCELTL2 16→0, TINYSTM 11→0, WT 21→0, WBETL 4→0,
   NOREC 10→0, NORECBF 13→0, LEFTRIGHT 19→0, ROMULUS 11→0, MVLOG 7→0, SWISSTM
   24→0, TSC_TM 17→0, XTM 6→0, TSXSGL 5→0, GPU_STM_CPU 2→0, SPHT 1→0, and 0
   throughout for SGL, JVSTM and CSMV.

   | backend | still open | where |
   |---|---|---|
   | — | none | every backend is race-clean under ThreadSanitizer |

   ACCELTL2 is gated on its CPU path (`TM_ACCEL_DISABLE=1`, where the same binary
   reports **0**). With its device path forced on, the same run reports 12 issues
   whose stacks name only `libamdocl64.so` and `libhsa-runtime64.so.1` — the AMD
   ICD and its HSA runtime, whose own worker threads are created during context
   setup — and no frame from `tl2.hpp`, `tl2_runtime.cpp` or `accel_probe.cpp`;
   they also reproduce with `-T 1`, where the STM layer has no threads at all.
   Chasing them found one genuine defect, now fixed (§32): `tm_exit()` freed the
   fine-grain-SVM guard table that `tl2::STM::g_guards` still pointed at, so the
   next `tm_init()` phase segfaulted inside `commit()` and the accelerator could
   never be re-acquired. `tests/concurrency/tsan-suppressions-opencl.txt`
   documents and suppresses the driver noise (12 → 2, both driver-internal). What
   the sanitizer sweep still cannot see: `tm_swap_runtime`'s publication hazard
   (below) and accelerator behaviour that needs a specific device.
- **RESOLVED 2026-11-26 — CSMV commit could reuse a published version node**:
  `real_tm_end()` reused `g_tx.writes[i].node` when non-null, which would have
  rewritten the `timestamp`/`value`/`next` of a node already visible in the
  chain (a lockless chain walk could see a version jump backwards or skip one).
  The branch was dead today — `begin_transaction()` clears the write set — so it
  was a trap for any future retry path; commit now allocates unconditionally
  (`csmv_cpu_runtime.cpp`). Verified: `test_tx` 120/120, `test_ds` 207/207,
  `bank -t 8 -n 50000` conserves money in both plain and `CSMV_SERVER_COMMIT=1`
  mode, driver `-T 8 -n 3000` seeds 1–3 and server mode with all counters 0, the
  csmv unit suite is green, ThreadSanitizer stays at 0 races in both modes, and
  `bank -t 8 -n 200000` holds at ~58 MB peak RSS (GC still reclaims).
- **CSMV GC slot pinning if a thread dies mid-transaction**: a thread that
  leaves a slot registered (killed without `tm_exit_thread`, or a `longjmp`
  that never reaches `csmv_reg_end()`) pins the low-water mark until some later
  thread reuses that slot. Bounded (4096 slots) and conservative, but unbounded
  version growth if it happens early. Consider a heartbeat/epoch or a
  per-slot thread-id + liveness check.
- **`tm_swap_runtime()` still writes the dispatch table while threads may be
  running** (`backends/tm_impl/common/tm_hooks.cpp`): unlike the §29 publication
  path, a phase switch *intends* to change the table, so it writes all 23
  pointers under `s_hook_mutex` while other threads dispatch through them
  locklessly — a torn swap could mix two backends' hooks in one call sequence.
  Today the only caller is `tests/explicit-api/test_phase_switch.cpp`, which is
  single-threaded, so it is latent. A correct multi-threaded phase switch needs
  either per-thread shadow tables (each thread picks its table at
  `tm_init_thread`, and a switch waits for a quiescence epoch) or a real RCU
  hand-off. Add a TSan CI case for it at the same time — nothing in the current
  matrices touches `tm_swap_runtime`.
- ~~**ACCELTL2 OpenCL/SVM path: 2 races + 1 thread leak**~~ → **fixed 2026-11-26
  (§32)** — the sanitizer residue was third-party (the AMD ICD/HSA runtime's own
  threads; 12 reports, none naming one of our sources, all reproducible with
  `-T 1`), but chasing it found a real defect on the same path: `tm_exit()` freed
  the fine-grain-SVM guard table that `tl2::STM::g_guards` still used, so the next
  `tm_init()` phase segfaulted inside `commit()` and the accelerator could never
  be re-acquired. Fixed by teardown ordering (`tl2::STM::accel_teardown()` before
  `accel::shutdown()`), re-acquisition in `init()`, and making the module's
  published state atomic. Pinned by `tests/concurrency/repro_accel_teardown.cpp`,
  which `tools/diff_backends.sh` now runs for every backend (it SKIPs where no
  fine-grain-SVM device exists, so on accelerator-free runners the pin is inert —
  the device-backed assertion needs hardware, and `nightly.yml`'s `accel-e2e` is
  written to say so rather than quietly testing the fallback).
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

### Event-log invariant checker: pairing fixed (§38); per-backend event vocabularies still open (P2, opened 2026-11-26)

`tools/stm_bug_tool/invariant_checker.py` implements 14 checks and had **no
command-line entry point at all** — no `__main__`, no argparse — so the usage its
own README documented (`--log FILE`) could not work and nothing could run it in
CI. It now has a CLI (`--log`, `--baseline`, `--events`, `--quiet`, `--strict`,
`--max-show`; exit 1 on any CRITICAL, 2 on warnings with `--strict`), and the
README's invariant table lists what is actually implemented (it listed seven
invariants, three of which — money conservation, counter monotonicity, no-lost-
updates — are not functions in the file).

To give it an input from a clean run, `drv_diff_ops` now dumps each worker's ring
under `DRV_EVENT_DUMP=1` (a `TM_EVENT_DUMP` no-op unless built with
`-DTM_EVENT_LOG`), and `benchmarks/cpp/Makefile` gained `EXTRA_CXXFLAGS` so such a
build does not have to clobber `CXXFLAGS` (which carries the `-I` paths). Verified
end to end: TL2, `-T 1 -n 300`, 5 806 events → 0 critical, rc 0; a hand-made log
with an orphan commit and an unmatched `TX_BEGIN` → 3 critical, rc 1 (so the gate
is not vacuous).

What was left has one half done and three new gaps measured (P2, opened 2026-11-26):

- ~~**Pair the lifecycle and lock-discipline checks per thread id.**~~ → **RESOLVED
  same day (§38).** Both checks now pair via `group_by_thread`, and
  `parse_event_log` returns `dump_starts` so every dump's wrap state is known instead
  of just the last header's. Measured on the log that exposed it (TL2, 52 dumps,
  driver oracles all zero): lock discipline 167 findings → **0**, lifecycle 9 → 5.
  The remaining 5 were not a checker bug: TL2's `commit()` logged no `TX_ABORT` on
  either conflict route, so those transactions genuinely had no end in the log — fixed
  in §38, which takes the two shapes it measured from 130 and 317 lifecycle criticals
  to **0** and makes every complete-history dump balance `TX_BEGIN` against ends
  exactly. A `-T 1` log had hidden all of this, which is why the README and
  `docs/DEBUGGING_BACKENDS.md` no longer recommend `-T 1` as a workaround.
- ~~**The fourteenth invariant could never fire.**~~ → **found and fixed the same day
  (§41).** `check_user_oracle` read `parsed["_raw_text"]`, which `parse_event_log` never
  sets, and searched for `TM-INVARIANT: FAIL`, a prefix no program in the tree prints — the
  real format is `INVARIANT: money conservation: FAIL …` (`fuzz_bank.cpp:108`), which the
  parser recognises but filed into a single slot the check never consulted, so a later PASS
  overwrote a FAIL. A/B on that real line: old code 0 findings, new code 1.
  `window_around_failure()` also dropped `dump_starts`, so the windowed copies the tool
  tells you to use for 100 k-event logs reported every wrapped log as unwrapped.
- **Keep the checker's contract gated — `make check-pytools`.** Added with §41: eleven
  one-defect fixtures asserting exact count *and* severity, plus the CLI's five exit-code
  cases, run by CI's `lint` job. The checker is still not a gate on *backend* behaviour (the
  three bullets below), which is exactly why its own verdicts needed pinning: nothing in the
  tree would have noticed if a check had gone silent. When you settle one of those bullets,
  the fixture that pins its current severity changes in the same commit.
- **`Abort causality` does not know TL2's vocabulary.** Now that aborts are logged it
  fires on clean TL2 runs: 96 warnings at `-S 1 -T 4 -n 200`, 316 at
  `-S 2 -T 4 -n 1000` and 263 at `-S 3 -T 4 -n 1000`, because `ABORT_CAUSAL_EVENTS` is
  `{READ_VERSION_CHECK, GAP_CHECK, WRITE_LOCK_ACQUIRE}` and TL2's conflict evidence is
  a failed `COMMIT_LOCK_ACQUIRE` (which logs nothing on failure) or a read-set version
  mismatch (also silent). Either TL2 logs the conflict reason at those two sites, or
  the check learns that the set is per-backend. Do not just widen the set: on the
  backends it was written for, it is a real check.
- **`Version ordering` calls TL2's normal behaviour a stale read.** 14–41
  `CRITICAL`s per clean TL2 log, pre-existing (§38's table shows it on both sources):
  the check flags `READ_LOCK_ACQUIRE` version > `TX_BEGIN` start version, but TL2
  validates *every* read-set entry at commit (its Step 5 comment says so), which is
  the `extend()` the check's own docstring describes. It needs to become
  backend-conditional — TL2 reads with a newer version by design, TinySTM-style
  backends must not.
- ~~**A `-T 4` NOrec log produces thousands of criticals**~~ → **half fixed (§39).**
  Measured before: `-S 3 -T 4 -n 300` gave 3 427 criticals for NOREC (1 246 lock
  discipline, 1 250 address validity, 935 version ordering) and 3 754 for NORECBF. Both
  of the big ones were NOrec logging the *clock value* in `addr1` and no `LOCK_RELEASE`
  at all; with the payload convention restored, lock discipline is **0** and address
  validity is 4 for both backends, and every remaining critical is `version_ordering`
  (1 371 / 2 031).
- ~~**`Version ordering` vs NOrec's moving snapshot — and an unresolved soundness
  question.**~~ → **investigated and cleared (§40): the check, not the backend, was
  wrong.** `tx->snapshot` can only advance through `validate()`, and `validate()` re-reads
  *every* logged address and `abort_tx()`s if any value changed (`NOrec.hpp:246-250`)
  before returning a clock it has re-checked for stability — so the returned snapshot
  carries a proof about the whole read set, and the commit CAS against it proves nothing
  committed since. Measured with a scratch-instrumented `NOrec.hpp` (restored
  byte-identically; the clean rebuild verified to carry 0 `g_dbg_` symbols and 0
  `NORECDBG` strings): snapshot advancements 895 / 3 406 / 6 804 / 26 408 / 9 753 across
  five shapes including `-M 2`-only transfers at `-T 8/16/32`, and the value-check abort
  count equalled the driver's `aborts=` in **every** row (61 / 349 / 1 227 / 5 308 /
  1 790) — that check is the only thing that aborts NOrec in these workloads, it fires
  thousands of times a run, and all four consistency oracles stayed 0 over the 112 120
  commits those five runs made.
- **`version_ordering` is now a WARNING and should become backend-aware instead.** The
  demotion (§40) stopped the 1 371 / 2 031 NOrec and NORECBF findings reading as CRITICAL
  — the three real logs measured are now `0 critical` (1 416 / 2 119 / 352 warnings) while
  the deliberately-broken fixture still yields 3 criticals. But softer is not the same as
  correct: the check should know which discipline a log's backend uses — the versioned-lock
  backends where `read_version > start_version` is a genuine stale read, versus the
  value-validated ones (NOrec, NORECBF, TL2) where it is normal traffic — rather than
  warning for everyone. Either name the discipline per backend in the tool and default to
  warning for unknown ones, or find a discriminator inside the log itself.
- **`check_address_validity` misclassifies the TM region as stack.** It flagged
  5 218 events / 152 addresses as "stack" for pointers in the mmap'd region
  (`0x7f…` range, region base + 0x1000). Harmless as a WARNING, but it buries real
  findings; classify against the region bounds `tm_region_allocator` knows. It is the
  1 254-finding component of the NOrec number above too.

### GPUTX lost committed writes as thread count rose — RESOLVED (found & fixed 2026-11-26, §35)

`drv_diff_ops` failed its additive-delta oracle on GPUTX at rates that tracked
the thread count (`-n 5000`, 200 seeds each, pre-fix): `-T 24` 36/200, `-T 32`
81/200, `-T 64` 196/200, while `-T 8` had never failed in 12 seeds — which is why
every existing CI shape (`-T 4`, `-T 8`) missed it entirely. Fixed by taking a
global commit lock across validate+apply (the pattern MVLog and LEFTRIGHT
already use) and re-checking write-lock ownership before publish; post-fix
0/200 at `-T 24`, `-T 32` and `-T 64` with identical commit counts and a 9–21 %
abort-rate increase. `docs/CORRECTNESS_FIXES.md` §35 has the root cause, the
three hypotheses that instrumentation excluded (out-of-transaction reads/writes,
the reentrant-lock shortcut) and the ordering argument for why the new lock
cannot deadlock.

Follow-ups this leaves open (P2, opened 2026-11-26):

- ~~**GPUTX still validates by value and still reads unsynchronised.**~~ →
  **RESOLVED same day (§37).** Fixed as sketched here — width-exact
  `tm_word::load_relaxed`/`store_release` at every transactional access, plus a
  `std::atomic<uint64_t>` commit counter per *8-byte word* (not per 16-byte lock
  slot: slot granularity cost 16–30 % more aborts for the same soundness, measured)
  bumped under the commit lock after the values land, read with the seqlock
  discipline and validated under the commit lock. Acceptance met: 0 data-race
  reports at `-T 4 -n 200` and `-T 8 -n 1000` over 12 seeds each (was 12 and 15),
  and the `-T 16/24/32/64` rate tables stay at 0/200. Cost: aborts up 27–29 % at
  identical commit totals. GPUTX is in `tsan-gate` now.
- **GPUTX does not finish under TSan at ≥16 threads, with or without §37.**
  `drv_diff_ops -S 1 -T 16 -n 400` under `-fsanitize=thread` hits a 200 s timeout on
  the §35-era build *and* on the version-stamp build (both `rc=124`), while `-T 8`
  shapes finish in under a second. So the backend's spin-then-abort path is
  superlinear once TSan's shadow memory is in play, and there is no wide-thread TSan
  evidence for it either way — `ci.yml`'s `tsan-gate` shapes stop at `-T 8` for every
  backend, which is why this does not block anything. Worth measuring clean-vs-TSan
  scaling before adding any high-thread TSan shape anywhere; if the design is the
  cause, the fix is a bounded spin with an abort instead of a spin loop (the
  `try_lock_write` path already aborts — the spin is what costs).
- **Give GPUTX a permanent regression leg.** It was absent from both
  `tools/diff_backends.sh`'s default backend list and the `diff-concurrency`
  job's lists, so the bug had no gate at all. Done: GPUTX is in the default list,
  in the push-side CI legs, and `SCENARIOS` gained a `-T 16 -n 1500` leg — the shape
  that actually distinguishes fixed from flaky for this bug class (it failed 18 % at
  `-T 24` before). `tsan-gate`, `ubsan-gate` and the ASan leak gate cover it too.

### MVLog: reclamation is outside its model, and the fold serialises every commit (P2, opened 2026-11-26)

Fixed the correctness half today (CORRECTNESS_FIXES.md §34: the fold skipped the
reclaimer's own slot and left committed memory one commit behind). Two things
remain:

- **The TLA+ model cannot see this class.** `docs/proofs/MVLog.tla` abstracts
  reclamation away (Implementation_notes.md §5/§7: the model uses a
  no-reclamation `dirty` set), which is exactly why §34 survived it. Extending it
  with a `mem` map plus a fold action whose range is `[wm, slot]` — and checking
  `AddrNotDirty ⇒ mem[addr] = newestCommitted[addr]` — would gate the invariant
  the fast path depends on. Small model, `docs/proofs/Makefile` already runs
  three models in the `tla-smoke` job.
- **The fold runs under `g_commit_lock`.** It scans the whole window
  (`kReclaimThreshold` = 16 384 entries) with nothing else able to commit; the
  measured reclaim events in a 16 k-transaction run each touched ~80–170 distinct
  addresses, but the scan itself is O(window) and head-of-line-blocks every
  commit for its duration. Amortising it (fold a bounded chunk per reclaim, or
  fold eagerly when an entry leaves the window) is a measurement-first change:
  time `bank -t 8` and the driver with the threshold at 1024 vs 16384 before
  picking.
- **Measured: that serialisation is a cliff in thread count, not a linear cost**
  (measured 2026-11-26, `drv_diff_ops`, identity `g_mvlog_hooks`, seed 1, wall clock):

  | shape | wall | commits | aborts (share) |
  |---|---|---|---|
  | `-T 8 -n 1200` | <1 s | 9 944 | 6 305 (39 %) |
  | `-T 16 -n 1200` | <1 s | 19 544 | 22 940 (54 %) |
  | `-T 32 -n 1200` | 25 s | 38 744 | 80 209 (67 %) |
  | `-T 48 -n 1200` | >200 s, stopped | — | — |
  | `-T 64 -n 1200` | >200 s, stopped | — | — |
  | `-T 32 -n 300` | 6 s | 9 944 | 19 257 (66 %) |
  | `-T 64 -n 300` | >118 s, stopped (37 min CPU) | — | — |

  Same work at quarter size finishes at `-T 32` in 6 s but does not finish at `-T 64`
  in 118 s, so the cost is per-thread-contention, not transaction count — consistent
  with every commit queueing behind an O(window) scan: the expected wait grows with the
  number of threads, and the abort share growing 39 % → 67 % means each retry costs the
  system more log entries, feeding back. Whether it terminates at `-T 64` at all is
  unmeasured (I stopped it, so no claim either way). No CI shape exceeds `-T 16`, so
  nothing is currently exposed to this; if MVLog is ever expected to scale, the chunked
  fold above is the fix to try, and `-T 32 -n 1200` at 25 s is the number to beat.

Coverage note for any future windowed-reclamation backend: a driver scenario must
claim **more slots than the backend's reclamation threshold** or it never
executes a reclamation. MVLog's is 16 384 slots; `tools/diff_backends.sh`'s
`-T 8 -n 2000` legs are sized for that and say so.

### Exit-time leaks: 12 of 22 backends are leak-clean and gated; 10 are not (opened 2026-11-26, ACCELTL2 measured 2026-11-26)

`ci.yml`'s `sanitizer` job now runs AddressSanitizer over `bank`/`test_tx`/`test_ds`
for **all 22** backends (none has a memory error) with LeakSanitizer enabled
per-backend only where it is clean (`leak_check: true`: NOREC, NORECBF, TL2, MVLOG,
CSMV, SGL, GACCO, GPUTX, TSC_TM, TSXSGL, GPU_STM_CPU, ACCELTL2). It was previously
`detect_leaks=0` globally, which is why none of this was visible.

ACCELTL2 was the last backend added to the ASan matrix (it had never been measured
at all) and it went straight to leak-clean: 0 reports and 120/207 plus
"PASS: Money conserved" on `bank`/`test_tx`/`test_ds` under `detect_leaks=1`, with
`TM_ACCEL_DISABLE=1` — the same setting `tsan-gate` uses, and the same caveat: this
host has no AMD IGP, so that measures the CPU path, and the device-backed SVM path's
allocations have never been leak-measured. If a GPU runner ever reports it leaking,
measure the device path rather than deleting the entry. Three owners,
measured on `bank -t 4 -d 1000` with `detect_leaks=1`:

| backend | leaked | owner (symbolised from the report) |
|---|---|---|
| LEFTRIGHT | 53.6 KB / 1079 | the harness: `expli::QueueExecutor` futures/`std::function` deques |
| ROMULUS | 71.4 KB / 326 | the harness, same pattern |
| XTM | 1.4 KB / 12 | the harness, `execute_async` state |
| SPHT | 524 KB / 8 | backend wait/epoch state (vector of `condition_variable`, strings) |
| SWISSTM | 456 B / 9 | the `OwnershipRecord*` hash table |
| TINYSTM / WBETL / WT | 130 KB / 31 | `thread_local` deferred- + retired-free lists dropped when a worker exits |
| CALVIN | (bank only) | per-thread `CalvinState` |
| JVSTM | 10.8 KB in `test_tx` | every committed `VBoxBody`, forever — see the next entry |

Two of these are worth real fixes rather than suppression:

- **TinySTM's free-deferral lists are `thread_local` and are dropped at thread
  exit** (`g_deferred_frees`, `g_retired_frees`, `g_deferred_frees_set` in
  `backends/tm_impl/tiny_stm/TinySTM_runtime.cpp`). A worker that queues frees and
  then exits without another commit loses them. Freeing them *at thread exit* is
  **wrong** — the deferral exists precisely because other transactions may still
  read the buffer. The safe shape: hand the departing thread's lists to a
  mutex-guarded global orphan list (its pointers are already in
  `g_retired_global_set`, so the "free exactly once" property holds), let
  `tm_flush_retired_frees()` retire entries from it as usual, and drain whatever
  is left in `tm_exit()`, where the contract guarantees no transaction is live.
  Same argument applies to CALVIN's `__thread CalvinState`.
- **`bank`'s own executor objects leak**, which also makes its memory numbers
  wrong; fixing the harness is a prerequisite for gating LEFTRIGHT/ROMULUS/XTM.

`tests/concurrency/asan-suppressions-jvstm.txt` exists for JVSTM (verified: 12 → 0
reports, `test_tx` 120/120) so a developer can run ASan on it meanwhile; do not add
entries to it for anything else.

### JVSTM has no version reclamation at all (P1, opened 2026-11-26)

`backends/tm_impl/jvstm/jvstm_runtime.cpp` allocates a `VBox` per tracked address
and prepends a fresh immutable `VBoxBody` on **every committed write**, with no GC
pass anywhere in the file (the only `free`-ish code is `real_tm_free` for TM
blocks). Measured on `bank -t 4`: peak RSS 856 KB at 20k transactions and 9.3 MB at
200k — i.e. ~48 bytes and a factor of **10.9x per 10x workload**, unbounded, while
MVLOG (44.5→44.8 MB), CSMV (57.4→57.9 MB) and SWISSTM (262.2→262.9 MB) stay flat
because they trim. `test_tx` under LeakSanitizer reports 10 808 bytes in 579
allocations, all from `get_or_create_vbox`.

This is the paper's reclamation pass being unimplemented, not a slip, so the fix is
a feature: reuse the pattern already proven for CSMV in §27–§28 — publish a lower
bound taken from an active-transaction registry *before* reading the clock, then
let a commit trim bodies older than that low-water mark (keeping the newest body at
or below it). JVSTM's bodies are already chained newest-first from
`box->head.load(acquire)`, and `find_body()` walks that chain, so trimming is
"snap the chain"; the hard part, as in CSMV, is that the low-water mark must come
from the registry and not from a clock read. Until then, JVSTM is not suitable for
long-running processes and stays out of the leak-gated list.

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

### ARM TME: buildable now, still no end-to-end run (found 2026-11-26)

- **Files**: `gem5_sim/configs/arm-tme-kvm.py`, `gem5_sim/workloads/tme/`,
  `gem5_sim/scripts/run-arm-tme.sh`, `nightly.yml` (`htm-workloads-compile`),
  status table in `gem5_sim/README.md`
- **Fixed 2026-11-26**:
  - the config's release (`ArmDefaultRelease.for_kvm()`) never included `"TME"`,
    and gem5 reads TME from the release in `src/arch/arm/isa.cc` and
    `ID_AA64ISAR0_EL1.TME` — so `TSTART`/`TCOMMIT`/`SEND` were undefined on the
    Timing CPU the config switches to after boot. It now calls
    `release.add(ArmExtension("TME"))`.
  - `workloads/tme/Makefile` asked for `-march=armv8.1-a` without `+tme`, and
    the ACLE TME intrinsics are only declared with the feature on. It now builds
    with `armv8.1-a+tme` and fails with an explanation when the compiler cannot
    do TME (notably clang: no `tme` feature through LLVM 22, `__ARM_FEATURE_TME`
    never defined — this needs `gcc-aarch64-linux-gnu`).
  - A weekly non-blocking `htm-workloads-compile` job compiles the RTM and TME
    workloads so neither can rot silently again.
- **Two of my initial claims here were wrong and are corrected**: the TME
  workload *sources* do exist (`workloads/tme/{array_sum,hash_table}.c`, using
  `__tstart`/`__tcommit`), and the config is not stuck on the KVM CPU — it is a
  `SimpleSwitchableProcessor(KVM → TIMING)` that switches after boot.
- **Remaining gap**: no ARM (or ALL) gem5 build exists in this repo —
  `setup.sh`/`build.sh` produce `build/X86_TSX` — and `scripts/run-arm-tme.sh`
  wants `gem5/build/ALL/gem5.opt` plus an ARM full-system resource
  (`arm-ubuntu-24.04-boot-with-systemd`), so the TME path has never been *run*.
  x86 TSX remains the reference path that works end to end
  (`docs/x86-tsx-real-validation.md`).
- **Next step**: build gem5 for ARM (`./build.sh all`, or a `KVM`+`ARM` target)
  on a machine with the time for it, run `scripts/run-arm-tme.sh`, and record
  the abort/commit counters the workload prints next to the x86 numbers.


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

### Rust runtime: `expect()`/`unwrap()` triage + hardening — DONE (2026-10)
- **Context**: ~81 non-test `.expect(`/`.unwrap()` call sites across 15
  `explicit_api/rust/workspace` crates. Panics on the library path abort the
  process instead of surfacing an error.
- **Outcome**: a full `Result`-returning public API was rejected — the backend
  entry points (`tm_begin`, `tm_commit`, `tm_read_*`, …) mirror the C
  `TMRealHooks` ABI, so they cannot return `Result`; the correct behaviour at
  that boundary is to fail fast. Instead:
  - every bare `.unwrap()` on the library path was removed: mutex locks are now
    poison-recovering (`unwrap_or_else(|e| e.into_inner())`, the existing
    codebase idiom — 68 uses), and remaining invariants (sim-thread state, active
    transaction, `Layout`, `try_into`) use `.expect("<actionable invariant>")`;
  - genuinely fallible I/O no longer panics: `addrspace::mmap_file` returns null
    on a NUL-containing path, and `tsx_sim::tm_exit`'s `TSX_SIM_RING_FILE` dump
    logs and continues on create/write failure;
  - `mvlog::tm_begin` uses `get_or_insert_with` instead of an init-then-unwrap.
  - Remaining bare `.unwrap()` are only in `#[test]` fns and the two binary
    `main`s (converted to `.expect("worker thread panicked")`).
- **Verification**: workspace 41 tests, simulator 112, clippy clean, fmt clean.

### Rust runtime: `#[allow(dead_code)]` audit — DONE (2026-10)
- Removed genuinely dead helpers: `try_lock`/`unlock` (dudetm, tl2 — superseded
  by direct `lock_at_index(..).try_lock_exclusive()` calls), `tx_active`/
  `tx_aborted` (tsxsgl), `dbg_enabled` (tsx_sim). Verified with default,
  `simulation`, and `simulation,stats` builds.
- The remaining `#[allow(dead_code)]` are on `TxState` fields that are written
  but only read under the `simulation`/`stats` features (or kept for struct
  symmetry), plus the `simulator/src/tm_model.rs` lock-inspection helpers; they
  stay because feature-gating makes them load-bearing in specific builds.

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

### macOS plugin-mode static-global bridging — RESOLVED (2026-10)
- **File**: `backends/tm_impl/tm_region_allocator/tm_region_allocator.cpp`
- **Was**: the `registerStaticTMSymbols()` bridge (review-04 B-30/B-21) consumed
  the plugin-emitted `tm_symbol_{count,addresses,sizes}` table via an
  *undefined weak* data reference — a GNU-ld/ELF feature the Mach-O linker
  rejects, breaking every explicit-API link on macOS.
- **Fix**: provide *weak definitions* of the table in the runtime (count = 0,
  empty arrays) instead of an undefined weak reference. Explicit-API builds
  link only the weak defaults (loop is a no-op); plugin builds additionally link
  the module's *strong* definitions, which win on both ELF and Mach-O. No
  linker flags, no platform guard. Verified on macOS arm64 (plugin suite 11/11,
  explicit `test_tx`/`test_ds`); a macOS CI job guards it.

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

### AccelTL2 follow-ups (opened 2026-11-25)

- **Clean-verdict fast path (P2)**: skipping CPU step-5 validation after a
  device "clean" verdict needs a guard-state epoch proof — the naive
  clock-unchanged check is unsound against transient writer locks (a writer
  can lock, then abort, returning the guard's version while the pre-scan's
  "clean" was taken mid-lock). Design in
  `backends/tm_impl/accel_tl2/Implementation_notes.md`.
- **Single command queue (P2)**: concurrent large commits serialize their
  GPU dispatches on one queue + mutex (`accel_probe.cpp` `dispatch_mu`).
  Fine for the current pre-abort-filter role; revisit if dispatches become
  frequent (per-thread queues or batched dispatch).
- **NPU path (research)**: XDNA-class NPUs stay detect-only; a real NPU
  offload needs the guard state in DMA-staged snapshots — different design,
  not zero-copy. Keep detection + rationale; re-probe only if a
  shared-VA NPU programming model (e.g. oneLevelZero-style SVM) lands.

### Dead `TM_DEBUG_ALLOC` tracker (`g_tl_debug_allocs`) (P2, found 2026-11-25)
- `backends/tm_impl/common/tm_region_allocator.hpp` declares
  `g_tl_debug_allocs` and prints it under `-DTM_DEBUG_ALLOC`, but nothing
  ever defines/updates it — the leak-tracker mode reports an empty map (and
  only compiles since the 2026-11-25 include-ordering fix). Either wire the
  tracker into `tm_region_malloc`/`tm_region_free` or delete it.

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
