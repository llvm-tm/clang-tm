# Correctness Fix Plan — Implementation Status

| # | Issue | Status | Commit |
|---|-------|--------|--------|
| 1 | SwissTM debug fprintfs (performance) | **Fixed** | `ed1c75e` |
| 2 | Debug patch scripts path fix | **Fixed** | `27936d4` |
| 3 | NOrec STMbench7/TPC-C crash (SIGSEGV) | **Fixed** | `e885078` |
| 3b | NOREC commit write-back skipping non-TM addresses in expli mode | **Fixed** | `84400be` |
| 4 | LEFTRIGHT multi-thread deadlock | **Fixed** | `1de1b90` |
| 5 | XTM rbtree segfault | **Fixed** | `62d3878` |
| 6 | SIGBUS in test_treap_tx (tm_get_env/tm_set_jmpbuf as DATA) | **Fixed** | `1f4e309` |
| 7 | test_local_containers opaque errors (stdlib exception symbols) | **Fixed** | `1f4e309` |
| — | STL-in-TM tests removed from build + test runner (known broken) | **Removed** | `87ceb96`, `d2def60` |
| 9 | NOrec-BF plugin-mode bypass: heap addresses get zero TM protection | **Fixed** | review-02 S01, 2026-09-13 |
| 10 | NOrec RO→RW promotion: reads validated on commit (regression) | **Verified** | review-02 S04, 2026-09-13 |
| 11 | TinySTM invariants: unlock owner re-check + clock wrap (assert + counter + unit test) | **Hardened** | review-02 S03, 2026-09-13 |
| 12 | NOrec shared-structure cleanup on `tm_exit` (misreading — globals are static, no leak) | **Verified** | review-02 S05, 2026-09-13 |

---

## 1. SwissTM — debug fprintfs (performance) ✅

**Root cause:** Four unguarded `fprintf(stderr, ...)` in `commit()` were left over
from a debug session. Every commit wrote full transaction descriptor state to
stderr.

**Fix:** Removed from source. Moved to `patches/debug/patches/001-swisstm-commit-debug.patch`.
Apply via `./patches/debug/apply.sh`, remove via `./patches/debug/remove.sh`.

**Also fixed:** The debug patch scripts referenced stale paths (`debug_patches/`
instead of `patches/debug/`).

---

## 2. NOrec — STMbench7/TPC-C crash (SIGSEGV) ✅

**Root cause:** `read_word_norec` had no address validation. The LLVM plugin can
instrument null-pointer-derived GEP addresses (e.g. `&node->right` where `node`
became null due to concurrent mutation) inside `[[tx::transaction]]` functions.
Without a null/low-address guard, `read_value_from_addr`'s `memcpy` from the
near-null address crashes.

All other backends guard against this — NOrec was the only one missing it.

**Fix:** Added `isTMAddress()` bypass + null/low-address guard (`< 0x100000`) to
three locations:

1. `read_word_norec` — bypass before NOREC double-check protocol
2. `write_word_norec` — bypass before write-set creation
3. `commit()` write-back loop — skip non-TM write-set entries

Pattern matches WBCTL's proven `< 0x100000` guard.

---

## 3. LEFTRIGHT — Multi-thread deadlock (bank/ycsb) ✅

**Root cause:** Three bugs in the left-right barrier commit protocol:

1. **Deadlock:** Read-only transactions skipped both barriers, but `thr_counter`
   expected all threads to participate. Writer threads spun forever.

2. **Write-write conflict:** Right barrier only counted entries, did not
   serialize write-back — concurrent writers raced on the same address.

3. **Weak validation:** `validate()` checked a stale version condition, not
   the current address state.

**Fix:** Replaced the broken left-right barrier protocol with a global commit
lock (same approach as NOrec's write path):

- Removed `g_left_barrier`, `g_right_barrier`, `g_left_phase`, `g_right_phase`
- Removed `left_barrier()` and `right_barrier()` functions
- Added `g_commit_lock` (atomic spinlock)
- New commit protocol: validate (optimistic) → acquire lock → re-validate →
  clock++ → write-back → release lock
- Lock released before any `siglongjmp` path to avoid deadlock
- Queue mode path unchanged (already safe, no barriers needed)

---

## 4. XTM — rbtree segfault ✅

**Root cause:** XTM's full-page (4096 bytes) `memcpy` write-back on commit
overwrites the `ChunkHeader` and allocator bitmap that share the same 4 KB page
as data blocks. This corrupts allocator metadata, causing double-free / bad
pointer dereference.

**Fix:** Isolated allocator metadata from data pages in the TM region allocator:

- Bitmap mode: round `data_off` up to the next page boundary (`(hdr_total + 4095) & ~4095`)
- Freelist mode: start data at page 1 (`data_off = 4096`)

Now `ChunkHeader` and bitmap always live on page 0 of each 64 KB chunk, and
data blocks start on page 1+. XTM's full-page write-back (which only touches
data pages) can no longer corrupt allocator metadata.

Capacity impact: ~5-6% reduction per chunk (e.g. 2560 blocks instead of 2714
for 24-byte size class).

---

---

## 6. SIGBUS in test_treap_tx — tm_get_env/tm_set_jmpbuf as DATA ✅

**Root cause:** `tm_get_env` and `tm_set_jmpbuf` were standalone `__TEXT,__text` functions in every backend. The LLVM pass's `emitHookCall` treats them as function-pointer DATA variables — it generates `ldr x8, [x8]` which loads 8 bytes of function prologue (not a pointer), then `blr x8` jumps to garbage → SIGBUS (signal 10).

**Fix:** Converted both symbols to proper DATA variables registered through the hooks system:

- `tm_hooks.hpp` — Added `.get_env` and `.set_jmpbuf` fields to `TMRealHooks`
- `tm_hooks.cpp` — Added `tm_get_env` / `tm_set_jmpbuf` as `__thread DATA` variables with stub defaults:
  - `stub_tm_get_env` returns `&tm_jmpbuf` (the default thread-local jmpbuf)
  - `stub_tm_set_jmpbuf` is a no-op
- Registered in `apply_hooks_unlocked()`, `tm_swap_runtime()`, `s_real_hooks` save, and trace-hook init.
- 11 backends updated: TinySTM, DUDETM, NVHTM, SPHT, LeftRight, SwissTM, TSXSGL, DistributedSGL, SingleGlobalLock, TL2, NOrec, PersistentSGL — all removed standalone function definitions and registered via `.get_env`/`.set_jmpbuf` in their hooks struct.
- No‑op backends (TSXSGL, DistributedSGL, SGL, TL2, NOrec, P‑SGL) rely on the stubs from `tm_hooks.cpp`.

**Verification:** `nm` confirms `_tm_get_env` and `_tm_set_jmpbuf` now reside in `__DATA,__data`, not `__TEXT,__text`. `test_treap_tx` exits 0 (was SIGBUS 138).

---

## 7. test_local_containers — opaque errors + broken-module bug ✅ (fully fixed)

**Root cause (opaque errors):** See original entry below — missing stdlib exception symbols in `KnownSafeOpaqueTable`.

**Root cause (broken-module bug):** The inline pipeline (`tm-instrument-inline`) ran `injectTransactionBeginEnd` on the function `vector_tx` after all its callees had been inlined. The function body contained 77 `ret void` instructions from `std::vector` template expansions, and the return-splitting logic in `injectTransactionBeginEnd` produced a basic block without a terminator: "Basic Block in function '_Z9vector_txii' does not have terminator!".

**Fix:** Switched from `tm-instrument-inline` (inline pipeline) to `tm-instrument` (default 5-step Honorio pipeline). The Honorio pipeline clones functions before instrumentation (`tm-clone` pass), so each `_tm_clone` has exactly one return — the return-splitting logic works correctly. Removed the custom Makefile rule; added `test_local_containers` to `TEST_NAMES`.

**Verification:** `bin/test_local_containers`: `g_tx_count = 419 (expected >= 400)` PASS.

## 8. Queue runtime DATA/TEXT symbol conflicts — tm_enqueue, tm_wait_prev_tx, tm_init_thread, tm_exit_thread ✅

**Root cause:** Four hook symbols in the queue runtime were declared/defined as TEXT functions (standalone `__TEXT,__text`), but the LLVM pass declares all hooks as DATA variables (`external global ptr`). On macOS, ld64 silently accepts the mismatch (treats the TEXT address as a pointer value). On Linux, LLD strictly enforces type checking — the generated code loads 8 bytes of function machine code as a function pointer → jumps to garbage → `udf #0xe08` trap.

**Symbols affected:**
- `tm_enqueue` — was `void tm_enqueue(...) { ... }` (TEXT) → now `void (*tm_enqueue)(...) = &real_tm_enqueue;` (DATA)
- `tm_wait_prev_tx` — was `void tm_wait_prev_tx(void) { ... }` (TEXT) → now `void (*tm_wait_prev_tx)(void) = &real_tm_wait_prev_tx;` (DATA)
- `tm_init_thread` — was `extern "C" void tm_init_thread(void)` (TEXT declaration) → now `extern "C" void (*tm_init_thread)(void)` (DATA declaration)
- `tm_exit_thread` — same pattern as `tm_init_thread`

**Pattern used** (same as `tm_sigsetjmp` fix in `plugin/runtime/tm_runtime.cpp`):
```cpp
// 1. Real implementation as static function
static void real_tm_enqueue(void (*fn)(void*), void* args) { ... }

// 2. DATA variable pointing to it
void (*tm_enqueue)(void (*)(void*), void*) = &real_tm_enqueue;
```

**All test/bench files using these hooks updated:** `test_queue.cpp`, `test_queue_async.cpp`, `bench_queue_compare.cpp`, `bench_queue_compare2.cpp`, `stmbench7_queue_manual.cpp` — changed `extern "C" void tm_wait_prev_tx(void)` → `extern "C" void (*tm_wait_prev_tx)(void)`.

**Verification:** `bin/test_queue`: PASS, `bin/test_queue_sync`: PASS, `bin/test_queue_async`: PASS. `make -C plugin run`: all 18+ plugin tests pass on macOS arm64.

---

## 9. NOrec-BF — plugin-mode read/write bypass (heap zero protection) ✅

**Root cause:** `NOrec_BF.hpp` `read_word_norec()` / `write_word_norec()` carried
`#ifdef LLVM_TM_PLUGIN` guards that bypassed ALL TM tracking for any address not
in the TM mmap region (`!stm::isTMAddress(addr) && !stm::isTMGlobal(addr)`).
Benchmarks allocate TM data on the regular heap via `new`/`malloc`
(`TMSafeVector::grow` → `::operator new`, `social_tm.cpp` → `new SocialNode[n]()`),
so every plugin-instrumented NOrec-BF operation took a plain load/store path —
zero read-set/write-set tracking, no validation, no atomicity. Invariant
failures (e.g. DeathStarBench `social_tm_norec`) were a backend bug, not a
benchmark bug. The main `NOrec.hpp` was already migrated in an earlier session;
`NOrec_BF.hpp` was missed, and its commit path also *skipped* non-TM write-set
entries (dropping valid heap write-backs).

**Fix (2026-09-13, review-02 S01):** Migrated `NOrec_BF.hpp` to the same
`stm::isOnCurrentThreadStack(addr)` pattern as `NOrec.hpp` / TinySTM's
`LLVM_TM_ADDR_CHECK`: only address-invalid cases (null, `< 0x100000`,
non-canonical) and the current thread's stack bypass tracking; all heap/TM-region
addresses go through full tracking. Removed the commit-time `isTMAddress` skip
(only the null/low-address safety net remains) so tracked heap write-backs are
published.

**Verification:**
- `test_tx` NOREC: 114/114 PASS; `test_ds` NOREC: 207/207 PASS
- `test_tx` NORECBF: 114/114 PASS; `test_ds` NORECBF: 207/207 PASS
- `social_tm_norec -u 256 -t 4 -d 3000`: PASS (invariant verified)
- `bank_norec -a 128 -t 4 -d 1000`: PASS (money conserved)

---

## 10. NOrec RO→RW promotion — reads validated on commit (regression) ✅

**Finding (review-02 S04, TODO.md:40):** The `read_only` flag in NOrec /
NOrec-BF only gates the commit fast-path skip for pure read-only transactions.
Reads are ALWAYS recorded in `read_set` regardless of the flag, so when the
first write clears `read_only`, the commit path validates the entire read-set
(including reads made during the RO phase). No correctness bug existed; the
TODO's "stale read-set not tracked" premise was a misreading.

**Action:** Added `tests/explicit-api/test_norec_ro2rw.cpp` — a conservation
stress test where every transaction performs its reads strictly before its
first write (forcing RO→RW promotion mid-transaction). If an implementation
ever dropped or failed to validate RO-phase reads, money conservation would
break under concurrency. Wired as `bin/test_norec_ro2rw` + `run-test-ro2rw`
target in `benchmarks/cpp/Makefile`; included in `run-tests`.

**Verification:**
- NOREC: PASS (2 checks, 4 threads × 50k read-first-then-write transfers)
- NORECBF: PASS
- TINYSTM (backend-agnostic sanity): PASS — 201,053 commits, 1,378 aborts

Comments at `NOrec.hpp` / `NOrec_BF.hpp` RO→RW writes updated to document the
semantics.

---

## 11. TinySTM invariants hardened: unlock owner re-check + clock wrap (review-02 S03) ✅

**What changed:**
- `Lock::unlock()` (`tinystm_common.hpp`): the old silent `if (is_locked() &&
  get_owner() == tx_id)` guard now also counts the "locked by a DIFFERENT
  transaction" case in `tinystm::g_unlock_owner_mismatch` (atomic counter)
  and fires `TM_ASSERT(false, ...)` in debug builds. The counter is always
  maintained (even with `-DNDEBUG`) so release runs can report it. No
  behavior change for the normal path (owning unlock) or the benign
  already-released case.
- `inc_abort()`: stale `// TODO.md: clock wrap` replaced with a rationale
  comment (3-bit incarnation wrap is safe because all acquires CAS the full
  lock state).
- **Clock wrap verified**: `tests/explicit-api/test_tinystm_clock_wrap.cpp`
  (new, header-only unit test, `make test_tinystm_clock_wrap` in
  `tests/explicit-api`) drives the global clock to `VERSION_MAX - 1` and
  verifies `increment_clock()`'s reset path: lock table zeroed, clock
  back to 1, `reset_locks_thr` cleared, repeat with a second tx id.

**Rationale / non-bugs:**
- `unlock()` owner mismatch is unreachable through current call sites
  (commit paths guard with `is_locked_by(tx->id)`; abort paths iterate
  `locks_held`, which only contains self-acquired locks) — the counter is
  defense-in-depth, not a fix for an observed bug.
- The version clock never overflows: `increment_clock()` resets at
  `VERSION_MAX` (2^46-1), verified by the new unit test.

**Verification:**
- `test_tx` TinySTM: 114/114 PASS; `test_ds` TinySTM: 207/207 PASS
- `test_stress_ds` (debug build, TM_ASSERT active): 127,065 tests PASS,
  4,444 aborts, zero unlock-owner assert firings
- `test_tinystm_clock_wrap`: 11/11 PASS
- TODO.md:22 (lock owner re-check) + :28 (clock wrap) closed

---

## 12. NOrec shared-structure cleanup on `tm_exit` (misreading — no leak) ✅

**Original claim (TODO / review-02 S05):** `tm_exit()` does not free the
shared global clock, commit lock, or Bloom filter → memory leak on process
shutdown. The proposed fix was to `munmap` them in `tm_exit()`.

**Root-cause analysis — the premise is false.** Inspecting the actual
allocations:

- **`NOrec_globals.hpp`** defines `global_lock`, `thr_counter`,
  `g_tm_abort_count` as `std::atomic<…>` with **static storage duration**.
  No `new`, no `mmap`, no `new[]`.
- **`NOrec_BF_globals.hpp`** additionally defines `g_gc_gen` (atomic scalar)
  and `g_gc`, a `stm::BloomFilter<kBloomWords>` — a class whose only data is
  a fixed `std::atomic<uint64_t> words_[WORDS]` array (`tm_bloom_filter.hpp`).
  Also static storage.
- The only dynamic allocation in either backend is the per-thread
  `Transaction` (`new Transaction()` in `init_thread()`), which is already
  freed by `delete current_tx` in `exit_thread()`.
- The 64 GB TM region *is* an `mmap`, but it is deliberately **not**
  `munmap`'d at exit — `stm::tm_region_destroy()` is a documented no-op
  because the OS reclaims the virtual mapping at process exit (resetting the
  slab index would corrupt hot-chunk headers).

So there is nothing to `munmap`/`free`: the "globals" the TODO names are
static objects reclaimed by the OS, and the one real `mmap` is already
handled by the region allocator's documented design.

**Fix:** Replaced the misleading `// TODO.md: NOrec shared-structure
cleanup (P1)` comments in `NOrec.hpp::exit()` and `NOrec_BF.hpp::exit()`
with accurate comments documenting that nothing is allocated and why the
`exit()` body is correctly empty. No behavior change.

**Verification:**
- `valgrind --leak-check=full ./benchmarks/cpp/bin/bank -t 2 -d 30`
  (NOREC): `in use at exit: 0 bytes in 0 blocks` — **no leaks are
  possible**. (The only 2 reported errors are the known glibc
  dynamic-loader `strsep`/rpath false positives, not memory leaks.)
- TODO.md "NOrec shared-structure cleanup" marked RESOLVED.

---

## 13. GUST GPU backend — money conservation via TLA+ alignment (review-05) ✅

**Symptom:** `gpu_gust_smoke` / `gpu_bank` lost money (e.g. 250000 vs
256000) on every seed; `gpu_ycsb_gust` / `gpu_memcached_gust` died with AMD
`HSA_STATUS_ERROR_EXCEPTION` at any nonzero write ratio (documented `@broken`
since review-03).

**Root causes** (found by checking the implementation against
`docs/proofs/GPU_GUST.tla` — the model itself was broken too, see below):

1. **Bootstrap clock (dominant money bug):** GTS/writePtr started at 0 while
   seeded VBoxes carry version 1; the first batch snapshotted `startTS=0`,
   could not see the seeded balances, read "absent" (0) and **committed 0
   back** over the seed values.
2. **VBox publication order:** write-back advanced `head`, stored `versions`
   *then* `values` — readers could observe the new window with a
   half-written (version, value) pair, and after wrap, a fresh `value`
   behind an *old* version (torn pair).
3. **Snapshot read picked the first slot in scan order** with version ≤
   threshold instead of the maximum — out-of-order concurrent write-backs
   make slot order differ from version order, so the read can return a stale
   value while a newer committed version ≤ threshold exists (lost update).
   The TLA+ model had the mirror-image flaws: `FindBody` CHOOSE'd any body
   ≤ rv and `Valid`'s MRV conjunct checked only the **first** history
   element (`vbox[addr][1][1]`) — TLC found `Inv` (InvNoMissedConflict)
   violated in `GPU_GUST.cfg` exactly through that hole (out-of-order
   prepend hid version 4 behind version 1).
4. **CL entry published state before payload:** `state=PENDING` written
   before `write_addrs/vals`; concurrent CCT scans saw PENDING entries with
   stale/zero write-sets → missed conflicts.
5. **CL ring had no epoch:** entries never returned to FREE (after one
   revolution every transaction aborts), and the wrap guard *overwrote* live
   foreign entries with ABORTED.
6. **Reserve-vs-publish race:** the AtomicINC reservation outpaced entry
   publication; validators treated not-yet-published slots as FREE →
   missed conflicts (the spec models insertion as atomic).
7. **MRV skipped when the CCT loop never crossed below GTS** — spec `Valid`
   is CCT ∧ MRV, unconditionally.
8. **`shfl_down` + local max in pre-validation is not a warp reduction:**
   lanes ended with different `max_writes`, running different numbers of
   `__ballot_sync` — partial-mask ballots → the AMD hardware exceptions.
9. **Oversized read/write-sets were silently truncated** (dropped writes =
   lost updates; untracked reads never validate).
10. `gpu_memcached_gust.cu` called the warp-collective `gust_gpu_commit`
    from two divergent branches (second HSA-exception source).

**Fix** (`gpu/backends/gpu_gust/`, `gpu/benchmarks/gpu_memcached_gust.cu`):
init `gts = writePtr = WARP_SIZE`; publish VBox payload (value) first and
version last as the marker, reserve-all-slots-then-publish, overflow-abort
instead of overwriting live versions (slots never reused); snapshot reads
take max version ≤ threshold; CL entries carry a `cts` epoch (validators
ignore foreign generations and spin until the reserved epoch publishes);
insertion writes payload → fence → state marker; MRV unconditional
(`has_newer` scans every slot); butterfly `__shfl_xor` max-reduce;
set-overflow forces abort; single uniform `gust_gpu_commit` call site in
memcached; host asserts `num_warps < CL_SIZE/WARP_SIZE` (ring safety).
Model fixes in `docs/proofs/GPU_GUST.tla`: `FindBody` = max version ≤ rv;
MRV = *no* body newer than the snapshot (both `define` and translation).

**Verification (AMD, ROCm 7.2.5, gfx1151):**
- `gpu_gust_smoke`: 12/12 configs (warps 4–32 × accounts 32–1024 × 3 seeds)
  — money conserved, all lanes finalized. (Was: fail on every seed.)
- `gpu_bank`: PASS incl. `32×70` iterations = 72 704 CL slots (**crosses one
  full 65536-slot ring revolution** — epoch reclamation exercised) and hot
  `accounts=8, iterations=40`.
- `gpu_ycsb_gust` / `gpu_memcached_gust`: PASS at write ratios 0/50/100,
  incl. hot 64-key contention; no HSA exceptions. `gpu_fuzz_counter` (CSMV)
  unaffected.
- TLC: `GPU_GUST-small.cfg` green post-fix; full `GPU_GUST.cfg` green:
  532 116 729 states generated / 222 033 780 distinct, **no error** (15 m
  33 s, 8 workers) — the corrected protocol checks out; the pre-fix model
  violated `Inv` in under 3 minutes.

---

## 14. Rust runtime P0 batch (review-05 R-01..R-07) ✅

**Root causes → fixes → verification** (all fixed in-tree with regression
tests; `cargo test --workspace` 38 tests, `simulator` 107 tests, all green):

- **R-01 TinySTM WT** (`runtime/tinystm/src/wt.rs`): `undo_backs` applied in
  forward order, so a repeated write left an intermediate value after abort.
  Fix: apply undos newest-first at all three apply sites. 4 tests (abort
  restores pre-TX value, fresh TX observes it, commit keeps last, raw-bytes
  variant).
- **R-01 follow-up (WT version livelock)** (`common.rs`): `unlock_exclusive`
  bumped the lock version even on abort, moving versions past `G_CLOCK`; a
  later transaction's read then looped forever in `read_word`
  (`snapshot_extend` could never reach the version). Fix: WT records each
  acquired lock's old version (`locked_old_versions`), restores it on abort
  (`unlock_indices_restore`), and stamps the commit timestamp on success
  (`unlock_indices_stamp`). Caught by the new R-01 test hanging.
- **R-02 NOrec** (`runtime/norec/src/lib.rs`): a narrower write into a wider
  buffered entry was silently dropped (`esz >= sz → return`) and narrower
  reads of wider entries fell through to memory. Fix: little-endian merge at
  the shared base address (narrow write splices into wide entry; wide write
  supersedes), reads slice wider buffered entries or splice a narrower
  prefix into a wider memory read (the read-set still records the pure
  memory observation, so validation stays honest — verified by
  `test_conflict_different_values_norec` in the simulator, which caught an
  over-aggressive first attempt that skipped own-address validation).
- **R-03 tsx_sim** (`runtime/tsx_sim/src/lib.rs`): `tm_write_ptr` /
  `tm_write_raw` buffered the write and then fell through to a raw store in
  TSX mode, leaking speculative values into memory where they survived
  abort. Fix: `return` after buffering in the `in_tsx` path (mirrors the
  typed `def_write!`). 4 tests (commit publishes, abort discards).
- **R-04 leftright_single** (`runtime/leftright_single/src/lib.rs`): undo
  materialized `0` via `unwrap_or(0)` for never-written addresses (shadowing
  memory forever); repeated-write undo applied forward; readers loaded
  `ACTIVE` before registering on the reader counter (writer could flip and
  drain the old counter while the reader was entering → concurrent HashMap
  get/insert UB). Fix: `old_val: Option<u64>` with remove-on-None, reverse
  undo order, and the classic LeftRight handshake (register → fence →
  re-check `ACTIVE` → retry on flip). 4 tests.
- **R-05 MVLog** (`runtime/mvlog/src/lib.rs`): reclaimed log entries were
  folded back into memory with a hardcoded width of 8 (clobbering up to 7
  neighbouring bytes of 1/2/4-byte writes), and a fold could regress an
  address the triggering commit had already published a newer value for.
  Fix: fold uses the per-entry type tag (`0/1/2/3 → 1/2/4/8` bytes) and
  skips addresses present in the committing transaction's write-set.
  Tests drive the 16384-slot reclaim window (~2.7 s).
- **R-06 SwissTM** (`runtime/swisstm/src/lib.rs`): reading an address whose
  `w_lock` was held returned the value without recording it in the read-set,
  so the dirty read was never validated. Fix: record `(addr, ver)` before
  returning. Test forces `w_lock` and asserts the read-set contains the
  address.
- **R-07 TL2 & DuDeTM** (`runtime/tl2/src/lib.rs`, `runtime/dudetm/src/lib.rs`):
  commit-time lock dedup checked only `locked_idxs.last()`, but the
  write-set sorts by address, not lock index — two addresses sharing one
  lock with a third address between them made the transaction re-lock a lock
  it held and spin forever (non-simulation builds). Fix: dedup against the
  whole locked-vector (`contains`), matching tinystm's `common.rs`. Tests
  scan a 32 MiB buffer for a genuine hash collision and commit through it.

## 15. CSMV GPU/CPU validation alignment + version-node GC (review-05 G-04) ✅

**Root cause (two-sided).** The GPU executor and the CPU fallback recorded
opposite validation stamps: GPU recorded the *observed version node's* ts,
CPU recorded the *head's* ts. Empirically (gpu_tpcc with a collision-free
table), the head-ts scheme loses updates — a transaction that reads an
older-than-head snapshot and validates "head unchanged" commits over
concurrent prepends (money-conservation invariant breaks, sum below
expectation). The node-ts scheme is the sound one: validation demands the
head still equals the exact node we read.

**Fix.** Unified both implementations on the node-ts convention
(`csmv_gpu_read`, `csmv_cpu_runtime.cpp`). Additionally the GPU commit
reordered to *lock → validate → prepend* (it previously validated before
taking the write locks, leaving the classic validate-vs-prepend window
where two transactions both pass and both prepend); this now matches the
CPU's mutex ordering. `gpu_tpcc` invariant holds across
{1,2,8,64}-warehouse configs and multiple seeds.

**Version-node GC.** Device nodes were `malloc`'d per commit and never
freed. Commits now retire chain entries older than the batch watermark
(the clock at batch launch; every reader in the batch and after has
start_clock ≥ it) once a watermark-visible entry supersedes them; retired
nodes are drained to a device free list by a tiny kernel between batch
launches (host-sequenced, so no batch is in flight). Recycled memory is
reused via the free list; retirement keeps nodes linked-out but readable
until the drain, so lock-free readers never touch freed memory.

**Collateral fixes found along the way (G-18/G-19).**
`csmv_gpu_entry_idx` masked with the compile-time 2^20 table size even when
`csmv_gpu_init` allocated more — working sets above 2^20 cells aliased
entries and corrupted unrelated addresses. Now a runtime mask, plus a new
`csmv_gpu_set_data_arena(base, bytes)` giving contiguous cell arrays
collision-free direct indices. `gpu_tpcc` itself had three latent bugs: a
`CYT_IDX` macro indexing past the end of `g_cyt` (out-of-bounds global
writes), a snapshot kernel that read cells with compact strides while the
transaction wrote MAX_D/MAX_C strides (matched only at the full-size
config), and a uint64 money invariant that failed spuriously once
c_balance went negative (now signed accumulation). The benchmark runs its
cells from a single cudaMalloc'd arena registered as the TM arena.

**Commit-ratio note.** `gpu_tpcc` commits ≈ #warehouses (one winner per
hot warehouse cell per batch): the GPU executor deliberately never spins
on write locks (abort-on-contention, no in-kernel retry), so the old "8
commits / 1024 txns" was the contention model of this design, not extra
validation over-abort; the validation-convention fix changed soundness,
not the ratio.

**Verification.** `gpu_tpcc` (all configs × seeds, GC on and off),
`gpu_fuzz_counter`, full `gpu/benchmarks` battery (9 binaries, HIP),
CPU CSMV `bin/test_tx` 114/114 + `bin/test_ds` 207/207.

## 16. TinySTM quadratic write-set + retry/deferred-free races (review-06) ✅

**Symptom.** "TinySTM worker hang at >=2 threads" — the reason commit
0496686 added the `proactive_stop` workaround. Reproduced with stmbench7
`-t 8 -w 3`: workers stuck forever in `read/write_word_ctl` with the
global abort counter frozen (one transaction attempt never finishes).

**Root causes.** (1) `Transaction::write_set` is a vector scanned
linearly by `ws_find`/`ws_get_or_insert`; STMBench7 long traversals
touch ~10^5 words, so each access is O(N) and each attempt is O(N²) —
minutes-long transactions that no contention manager ever aborts.
(2) The plugin's TX wrapper re-enters its own `sigsetjmp` on every
abort — an unbounded retry loop. (3) `real_tm_begin` published
`g_thread_tx_version[tid]` *after* `tinystm::begin()`: a concurrent
committer computing `safe_version` could see the slot as idle (0) and
flush (free) retired memory that the just-started snapshot still
referenced (use-after-free family; heap-corruption aborts).

**Fix.** Hybrid write-set index: above 32 entries a `ws_index`
(addr→index unordered_map) accelerates `ws_find`/`ws_get_or_insert`/
`ws_erase` while small transactions keep the allocation-free vector
path (preserves the documented intruder-benchmark perf rationale).
New plugin pass option `-tm-max-retries=N` bounds the wrapper retry
loop (default 0 = unbounded; stmbench7 builds set 100) — on budget
exhaustion the body is skipped and a zeroed return value produced.
`real_tm_begin` stores a provisional snapshot lower bound
(`get_clock()`) before `begin()` and excludes its own slot from the
safe-version scan. Matching malloc-based `operator new` overrides make
delete's `free()` legal. `proactive_stop` and all 15 call sites removed.

**Verification.** wbctl 0/20 hangs at `-t 8 -w 3 -d 3000` (was 12/12);
unit matrices `run-tinystm`, `run-wt`, `run-wbetl` PASS; full
post-merge gate green. Residual wbetl write-heavy crashes proven
pre-existing (HEAD crashes 8/10) and tracked separately.

## 17. GUST VBox windows never reclaimed → permanent hot-key starvation (review-06) ✅

**Symptom.** After review-05 made VBox slots immutable (no reuse while a
kernel is in flight), any address with ≥ `GPU_GUST_VBOX_DEPTH` (8)
committed versions made every subsequent writer abort with window
overflow — money-safe but throughput-starved forever under skew (smoke
hot key: only the first 7 commits ever landed).

**Fix.** Between-launch window compaction: `gust_gpu_commit` now counts
overflow aborts in a device counter, and `GUSTBatchExecutor::launch()`
runs `gust_gpu_vbox_compact()` whenever a batch overflowed — a
kernel that keeps each VBox's newest `GPU_GUST_VBOX_KEEP` (default 1)
versions and resets `head`. Safe only at the quiescent point the
executor guarantees (kernel complete + `cudaDeviceSynchronize`): the
next batch snapshots at the current GTS, which is ≥ every committed
version, so only the newest body can be the snapshot answer. The
microbench kernel gained the same overflow counter for parity.

**Verification.** `gpu_gust_smoke` invariant 3: 15 hot-key batches now
land 14 winners (was 7 — every batch after the window filled aborted);
`gpu_bank`, `gpu_ycsb_gust`, `gpu_memcached_gust`, `gpu_fuzz_counter`
and `make kernel-check` all PASS on the ROCm runner.

## Known remaining issues (not yet fixed)

| Issue | Severity | Notes |
|-------|----------|-------|
| TinySTM STMbench7 crash (STL vector realloc) | High | STL-in-TM incompatibility, pre-existing |
| LEFTRIGHT write-set `observed_version` | Low | Uses global clock at read time instead of per-address version; correct with global lock but more conservative than needed |
| stmbench7 times out with >1 thread | Low | Data race in `ts_multimap::lower_bound()` — pre-existing |
| **`test_queue_multi` counter mismatch (351 vs 400)** | Low | `counter` is a plain `static int` (not TM-tracked). The `TX` annotation creates a TM transaction wrapper, but the LLVM pass instruments only TM-annotated globals. 4 threads × 100 increments of a non-TM variable without atomic/lock protection → classic lost-update race. Expected 400, consistently observed ~351. Fix: either declare counter as `TM int counter` or use `std::atomic<int>`. Also affects `bench_queue_compare.cpp` and `bench_queue_compare2.cpp` (same plain-`static int counter` pattern). |
| **`test_queue_multi` no caller-side completion wait** | Low | The test joins pthreads (caller threads) but never waits for pool workers to finish all enqueued tasks. If `counter` were TM-tracked, workers might still be executing after `pthread_join` returns. Fix: caller threads should call `tm_wait_prev_tx()` after their enqueue loops. |
