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

## 18. TinySTM deferred-free EBR gaps: in-TX `std::free` UAF + re-defer cycling (review-06) ✅

**Symptom.** STMBench7 `-w 3` segfaults (jump to small-int PC, corrupted
saved rbp/return addresses on live worker frames) and glibc
`double free or corruption (!prev)` aborts; `test_simple_vector`
(plugin TinySTM link) stalls after ~5 s of growing slowdown.

**Root cause 1 (UAF).** `tm_delete_impl` / `real_tm_free` freed
non-region heap pointers immediately while the caller was inside a
transaction.  A container `grow()` (e.g. `TMSafeVector::grow`) frees
its old buffer at that moment, while other active transactions'
read-sets still dereference it — classic read-after-free feeding glibc
chunk reuse with wild pointers.

**Root cause 2 (cycling stall).** After routing in-TX frees through
the deferred/retired lists, the flush paths (`tm_flush_deferred_frees`
/ `tm_flush_retired_frees`) executed the final free as
`::operator delete(ptr)` — which re-enters the global
`operator delete` override (`tm_delete_impl`) while `g_in_tx` is still
set (flush runs from `real_tm_begin`).  The final free was therefore
re-deferred every cycle: each pass adds a bookkeeping node, the list
grows O(n) per transaction and every operation becomes O(n²) — the
"hang".

**Fix.** (a) `tm_delete_impl`/`real_tm_free` now defer non-TM pointers
onto `g_deferred_frees` when `g_in_tx`, retiring via EBR
(`tm_move_deferred_to_retired` at commit, freed once no older
transaction exists).  (b) The flush paths free buffers with
`std::free()` directly — correct pairing for the malloc-based
`operator new` override and immune to override re-entry.  (c)
`real_tm_begin` registers the thread's stack bounds
(`tm_record_stack_bounds`) on first transaction so
`isOnCurrentThreadStack()` bypasses work (without them, worker-stack
words were treated as TM data and abort rollback restored bytes of
live frames).  (d) `write_word_etl` now aborts when it ends up without
lock ownership instead of recording an unowned write (defensive).

**Verification.** `test_simple_vector` 3/3 PASS; TinySTM unit matrices
`test_tx` (114) PASS for WBCTL/WBETL/WT and `test_ds` (207) PASS;
STMBench7 wbctl 0/8 clean.  wbetl/wt `-w 3` remain crash-prone —
residual TB-lock data races tracked in TODO (previously masked by the
cycling bug never executing real frees).

## 19. TSXSGL SGL-entry store-buffer race: lost updates on real TSX (2026-10-05) ✅

**Symptom.** On real TSX hardware (intel14v2, E5-2660 v4), `fuzz_counter
BACKEND=TSXSGL` 4 threads lost 2–28 increments in **12/12** runs (2 threads:
2/3), and `bank -t 4 -d 1000` intermittently printed `FAIL: Money destroyed`.
All bounded gem5 runs of the same binary passed — the Ruby HTM model has no
store-buffer visibility window, so it cannot expose this class of race.

**Root cause.** `real_tm_begin()`'s SGL fallback stored `sgl_owner=1` with a
plain store and then immediately touched transaction data. The store can sit
in the core's store buffer while its loads/stores on shared data execute; a
concurrent TSX transaction that had loaded `sgl_owner==0` earlier is never
invalidated in time, commits, and the SGL core's stale-based stores land
afterwards → lost update. The `sgl_owner=0` store was equally unprotected,
but the mutex serializes SGL↔SGL, so only the *entry* fence matters.

**Fix.** `_mm_mfence()` after `sgl_owner.store(1, release)` and before the
body runs (`backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp`): drains the lock
word globally visible before touching data, which invalidates any
in-flight TSX transaction that read `sgl_owner==0` and forces it to abort
at commit.

**Isolation evidence** (`benchmarks/tsx/abl*.c` ablations, intel14v2):
pure-TSX 3/3 PASS, pure-mutex 3/3 PASS, full interlock 8/8 LOST (4 threads,
800k RMW), mfence-after-entry 8/8 PASS, mfence-after-clear 8/8 LOST.

**Verification.** After the fix: `fuzz_counter` 4×10k 10/10 PASS, 2×50k
5/5 PASS; `bank -t 4 -d 1000` 10/10 PASS; `bank` bounded t{1,2,4} PASS;
`test_tx` (114) + `test_ds` (207) PASS. Instrumentation (`TM_TSX_STATS=1`
env, per-thread counters merged at `tm_exit_thread`) added alongside —
counters must stay thread-local: an atomic incremented inside the live
transaction makes the counter line itself a conflict source.

## 20. Region-allocator `realloc` over-read and lost contents ✅

**Symptom.** `real_tm_realloc` in 11 backends copied into the new block the
number of bytes given by the *new* size (`memcpy(p, ptr, size)`), reading past
the end of the old allocation whenever it grew — an out-of-bounds read that
ASan flags (heap-buffer-overflow) but a plain run usually survives. TinySTM's
variant didn't copy at all: it zeroed the new block and freed the old one,
silently discarding the contents.

**Root cause.** Each backend re-implemented realloc instead of using the
region allocator's correct helper (`stm::tm_region_realloc`, which copies
`min(old_size, new_size)`).

**Fix.** Route every `real_tm_realloc` through `stm::tm_region_realloc`.
Backends that track speculative allocations call `tm_untrack_spec_alloc(ptr)`
first so a later abort does not double-free the replaced block; GACCO/JVSTM,
which never freed the old block, now do.

**Files.** `backends/tm_impl/{norec,tl2,swisstm,tsc_tm,mvlog,tiny_stm,
eager_broken,power8_htm,nvhtm,gacco,jvstm}/*_runtime.cpp`.

**Regression test.** `test_tm_realloc` in `tests/explicit-api/test_tx.cpp`
grows and shrinks a TM allocation inside a TX and checks the prefix survives;
it runs under the CI ASan job (NOREC/TL2).

**Verification.** `test_tx` 120/120 for NOREC/TINYSTM/TL2/SWISSTM/TSC_TM/
MVLOG/NORECBF/CSMV; NOREC ASan build clean; `test_ds` 207/207; bank money
conserved.

## 21. TM-region retire set never erased → permanent region leak → fuzz OOM (2026-11-25) ✅

**Symptom.** CI `fuzz-smoke` red since `3b5e289`: libFuzzer exit 71 (OOM) in
`test_tx_fuzzer` at ~8.4 M execs against the 2 GB limit. Reproducible locally:
the TM region burned ~34 slabs / 2 M driver iterations.

**Root cause.** Two independent leaks in the deferred-free pipeline:

1. `tm_flush_retired_frees()` erased **heap** pointers from
   `g_retired_global_set` after `::free`, but never erased **TM-region**
   pointers after `tm_region_free`. The region allocator recycles addresses,
   so the next time a reused address was retired,
   `tm_move_deferred_to_retired()` saw it in the set ("already retired by
   another thread") and dropped it — the block was never freed again. Every
   alloc/free cycle through a recycled address leaked its block permanently.
2. The fuzz harness itself fought the region allocator: it built 4 fresh
   `expli::TM<int>` objects per fuzz iteration (`TM<T>` values live forever
   in the region — no per-object destructor path), never freed `TM_malloc`
   results, and left transactions open across inputs (suppressing the
   deferred-free flush that happens at `begin()`/`clear()`).

**Fix.**

1. `backends/tm_impl/common/tm_alloc_overrides.hpp`: erase the pointer from
   `g_retired_global_set` in the TM-region branch of `tm_flush_retired_frees()`
   too (mirrors the heap branch).
2. `tests/fuzz/test_tx_fuzzer.cpp` rewritten: the four `TM<int>` objects are
   `static` (constructed once); each input runs inside one outer
   `expli::TM<int>::transaction()` frame (so the sigsetjmp retry frame and the
   op stream share a frame, and no op observes an inactive tx); nested `end()`
   only when `tm_nested_call_counter > 1`; `TM_malloc` results are `tm_free`d
   inside the transaction (deferred release at commit).
3. Incidental: `backends/tm_impl/common/tm_region_allocator.hpp` `#include`d
   standard headers *inside* `namespace stm` under `TM_DEBUG_ALLOC`, so the
   repo's own leak-tracker mode never compiled. Includes hoisted to file
   scope. (The `g_tl_debug_allocs` tracker it declares is itself a dead stub
   — tracked in TODO.md.)

**Verification.** Slab burn 34→23/2 M iterations (allocator-only driver);
both fuzz targets exit 0 repeatedly at the CI 30 s budget (~100 MB RSS) and
at a 120 s soak; `test_tx` 120/120 + `test_ds` 207/207 for TINYSTM/NOREC/TL2
unchanged; `make check-fast` green apart from the pre-existing CSMV failure
(separate row in the table below).

## 22. Switching `BACKEND` silently reused the previous backend's binary (2026-11-25) ✅

**Symptom.** A differential sweep script that looped
`make bin/drv_diff_ops BACKEND=$b; ./bin/drv_diff_ops` reported the *same*
violation counts for three different backends, and a follow-up "control"
run reported CSMV clean while a later run of the same command on the same
commit failed 79/144 tracked objects.

**Root cause.** `benchmarks/cpp/Makefile` links each binary directly from
sources (no per-backend object files), with file prerequisites
(`$(RUNTIME)`, sources, `Makefile`). Switching `BACKEND` changes which
files those prerequisites *name*, but the previously linked binary is
already newer than every prerequisite of the *new* backend, so make
declares the target `"up to date"` and does nothing — the old backend's
code keeps running under the new backend's name. Verified directly:

```
$ make bin/test_tx BACKEND=TL2 >/dev/null
$ make bin/test_tx BACKEND=SGL
make: 'bin/test_tx' is up to date.
$ nm -C bin/test_tx | grep -c 'tl2::'
23          # ← still TinySTM-free TL2 code despite BACKEND=SGL
```

Only `make -B` or `make clean` avoided it, so any
`for b in …; do make … BACKEND=$b; ./bin/…; done` loop in docs, scripts or
a shell session can produce measurements attributed to the wrong backend.

**Fix.** `benchmarks/cpp/Makefile` now maintains a stamp file
(`bin/.backend-$(BACKEND)`) that every link rule depends on; the stamp rule
deletes any existing `.backend-*` stamp and writes the current name, so a
backend change creates a prerequisite newer than every linked binary and
forces the relink. Same-backend rebuilds still no-op (verified: 1 compile
per switch, 0 on repeat), and `make clean` (`rm -rf bin`) clears it.

**Verification.** TL2 → SGL → TL2 switches each relinked and each binary
matched its backend (`nm -C bin/test_tx | grep -c 'tl2::'` = 0 for the SGL
build, 23 for the TL2 build); repeat invocations of the same backend
compiled nothing. Re-running the 21-backend differential sweep after the
fix produced per-backend-distinct results.

**Files**: `benchmarks/cpp/Makefile`.

## 23. JVSTM committed every write as 8 bytes → destroyed neighbours (2026-11-25) ✅

**Symptom.** The T1 differential driver (`tests/concurrency/drv_diff_ops.cpp`)
reported 51–61 of 144 tracked objects whose final value disagreed with the
committed-delta model on every seed, many of them sitting at exactly `0`
where the model said ~1000, plus several hundred reads far outside the
legal concurrent-commit window. `test_tx` (120) and `test_ds` (207) both
passed, and JVSTM is not in CI's `cross-backend` list, so nothing else
covered it.

**Root cause.** `backends/tm_impl/jvstm/jvstm_runtime.cpp` versioned
locations without tracking access width: `JvstmWriteEntry` was
`{addr, value}`, `get_or_create_vbox()` seeded the box with
`memcpy(&initial_val, addr, sizeof(uint64_t))`, and commit wrote through
with `memcpy(we.addr, &we.value, sizeof(uint64_t))` — for **every** width.
A committed `tm_write_i1` therefore stored its byte zero-extended into an
8-byte slot and wrote 8 bytes back, zeroing up to seven adjacent bytes; a
committed `tm_write_i4` zeroed the following int32. Reads over-read the
same way (an 8-byte load can also run past a small allocation).

**Fix.** Width became part of a version box's identity: boxes are keyed by
`(address, width)` (`box_key()`), read/write entries carry `width`, the
seed load and the write-through `memcpy` use exactly `width` bytes, and
read-own-writes falls back to the low `width` bytes of a wider queued write
to the same address. Mixed-width access to one address now gets separate
boxes instead of a shared 8-byte slot.

**Verification.** `tests/concurrency/repro_width_clobber.cpp` (new, minimal):
before — writing `c[3]` zeroed `c[4]`; one 1-byte commit zeroed `b[6..12]`;
a halfword commit zeroed `s[3..5]`. After — clean, and identical to the
TL2/SGL reference output. JVSTM `test_tx` 120/120, `test_ds` 207/207, and
the differential driver is now `quiesce_viol=0 v_range=0` (was 51–61 / up
to 575) on `-T 4` and `-T 8`.

**Files**: `backends/tm_impl/jvstm/jvstm_runtime.cpp`,
`tests/concurrency/repro_width_clobber.cpp`.

## 24. CSMV/GPU_STM_CPU read/write sets silently dropped overflow entries (2026-11-25) ✅

**Symptom.** CSMV's `test_tx` reported 1/120 failing — the check being
`tm_realloc tail writable`, i.e. after growing a 16-element block to 64
inside a transaction, `read_at(&q[63])` did not return what the same
transaction wrote. GPU_STM_CPU additionally failed the differential driver's
allocator wave (`alloc_viol=64/64`).

**Root cause.** Neither backend's allocator (both call the shared, already
fixed `stm::tm_region_realloc`) nor its `realloc` hook: the per-thread
transaction sets were fixed arrays whose insert paths read

```c
if (g_tx.num_writes < CSMV_MAX_WRITES) { /* record */ }   // else: dropped
```

so a transaction touching more than 32 distinct words lost every write past
the 32nd — and a transaction past 64 reads lost validation for those
objects. The failing test writes 16 + 48 = 64 words in one transaction.
Dropping is silent: no abort, no error, just an update that never happens.
(Instrumenting the pre-fix code confirmed the mechanism, and also showed
the driver's own transactions peaked at 4 reads / 4 writes — i.e. the
driver's remaining CSMV failures are a *different* bug, see the
word-aliasing row below.)

**Fix.** The sets grow: `std::vector` in
`backends/tm_impl/csmv/include/csmv_api.h` (`CSMVReadEntry`/`CSMVWriteEntry`)
and `backends/tm_impl/gpu_stm/cpu/gpu_stm_cpu_runtime.cpp`
(`ReadEntry`/`WriteEntry`), with the old `*_MAX_READS`/`*_MAX_WRITES`
constants kept as the per-transaction `reserve()` so the hot path stays
allocation-free for ordinary transactions. `begin()` clears the vectors (not
just the counters) and the commit-time sort runs over the vector range.

**Verification.** CSMV `test_tx` **120/120** (was 119/120) and `test_ds`
207/207; GPU_STM_CPU 120/120 and 207/207 with
`drv_diff_ops … alloc_viol=0` (was 64/64) at `-T 1` and `-T 4`.

**Files**: `backends/tm_impl/csmv/include/csmv_api.h`,
`backends/tm_impl/csmv/cpu/csmv_cpu_runtime.cpp`,
`backends/tm_impl/gpu_stm/cpu/gpu_stm_cpu_runtime.cpp`.

## 25. CSMV version store aliased word-sharing objects (2026-11-25) ✅

**Symptom.** Purely sequential, zero contention, zero aborts.
`tests/concurrency/repro_bucket_alias.cpp` puts 16 int32 objects in one
TM-allocated block and adds 1 to each in its own transaction: CSMV reported
**40/48 transactional reads wrong** and **16/16 committed values wrong**
(`c[0]` reads `c[1]`'s value). The differential driver reported
`quiesce_viol≈78-80` of 144 objects already at `-T 1`, plus ~200
out-of-window reads. Every one of the 20 other backends was clean on the
identical seed.

**Root cause.** `csmv_get_entry()` mapped an address to a table slot with
`(uintptr_t)addr >> 3`. That shift drops bit 2, so the two int32 halves of
any 8-byte word — and every sub-word object packed inside those 8 bytes —
landed in the SAME entry, and `CSMVVersionNode {timestamp, value, next}`
stored no address to tell them apart. They therefore shared a single version
chain, and `read_common()`'s fast path returned `entry->head->value`: the
value of whichever word-mate committed most recently. A read-modify-write
then wrote that foreign value back, corrupting committed memory, not just
reads. (The old code's own comments — "same hash bucket", "different
addresses may map to the same entry" — acknowledged the collision but the
per-entry validation and version chains silently relied on it not mattering.)

**Fix.** The table is now a real open-addressed map keyed by the exact
address. `CSMVObjectEntry` gained `std::atomic<void *> owner` (nullptr =
unused). `csmv_get_entry()` hashes the full address with a multiplicative
hash (so low-bit neighbours scatter), then linearly probes: an entry whose
`owner == addr` is returned, an empty entry is claimed via
`compare_exchange_strong`, and any other owner means "probe on". A probe
limit (1024) **aborts loudly** rather than falling back to a shared entry.
Entries are claimed once and never released — a region address reused by the
allocator maps back to the same entry — so there are no tombstones and no
rehash. Commit lock-ordering (sort by entry pointer) and read-set validation
are unchanged; `write_common`'s dedup and the lock-skip guard were updated
now that entry↔address is one-to-one.

**Verification.** `repro_bucket_alias` now clean; `drv_diff_ops` clean at
`-T 1` for **every** op-class mask (M = 1/2/4/8/16) on seeds 1-3, and clean
at `-T 4`/`-T 8` on the full mask; `test_tx` 120/120 and `test_ds` 207/207.
A **separate, rare** contention anomaly remains (see the known-issues table);
it was previously hidden by this one's mass corruption.

**Files**: `backends/tm_impl/csmv/include/csmv_api.h`,
`backends/tm_impl/csmv/cpu/csmv_cpu_runtime.cpp`,
`tests/concurrency/repro_bucket_alias.cpp`.

## Known remaining issues (not yet fixed)

| Issue | Severity | Notes |
|-------|----------|-------|
| TinySTM STMbench7 crash (STL vector realloc) | High | STL-in-TM incompatibility, pre-existing |
| ~~CSMV `test_tx` fails 1/120~~ → **fixed 2026-11-25 (§24)** | — | Fixed-size read/write sets (64 reads / 32 writes) that **silently dropped** overflow entries. `test_tx`'s `tm_realloc` case writes 16+48 words in one transaction, so its tail write never reached memory (the `TM<int*>`/`thread lifecycle` FAIL labels were `test_tx`'s cumulative counter bleeding). Same pattern in GPU_STM_CPU, where it also tripped the differential driver's allocator wave. CSMV now 120/120, GPU_STM_CPU 120/120 + driver clean. |
| ~~CSMV version store aliases word-sharing objects~~ → **fixed 2026-11-25 (§25)** | — | Version chains were keyed by `(addr >> 3)` with no address tag, so word-sharing objects shared a chain and read back each other's values (which a read-modify-write committed). `repro_bucket_alias` was 40/48 reads + 16/16 committed wrong at `-T 1`. Fixed by an owner-tagged, open-addressed, exact-address table. Clean now at `-T 1` for all masks and `-T 4/-T 8` full mask; `test_tx` 120/120, `test_ds` 207/207. |
| **CSMV rare lost-update / phantom under contention** | Medium | After §25, a **rare** anomaly survives: `drv_diff_ops` reports `quiesce_viol` 1–4 on ~1 in 3 `check-diff`-scenario runs (more with larger `-n`), deltas off by 1–5 in both directions, `quiesce_viol` the only nonzero counter, `-T 1` always clean. Isolated to CSMV (TL2/SGL/NOREC clean on the same failing seeds); GC/reclamation ruled out (still reproduces with reclamation disabled). Lives in the OCC commit path / lockless read validation. `tools/diff_backends.sh` reports it known-open for CSMV (nonzero `quiesce_viol` with all other counters zero) without gating it. Tracked in TODO.md. |
| LEFTRIGHT write-set `observed_version` | Low | Uses global clock at read time instead of per-address version; correct with global lock but more conservative than needed |
| stmbench7 times out with >1 thread | Low | Data race in `ts_multimap::lower_bound()` — pre-existing |
| **`test_queue_multi` counter mismatch (351 vs 400)** | Low | `counter` is a plain `static int` (not TM-tracked). The `TX` annotation creates a TM transaction wrapper, but the LLVM pass instruments only TM-annotated globals. 4 threads × 100 increments of a non-TM variable without atomic/lock protection → classic lost-update race. Expected 400, consistently observed ~351. Fix: either declare counter as `TM int counter` or use `std::atomic<int>`. Also affects `bench_queue_compare.cpp` and `bench_queue_compare2.cpp` (same plain-`static int counter` pattern). |
| **`test_queue_multi` no caller-side completion wait** | Low | The test joins pthreads (caller threads) but never waits for pool workers to finish all enqueued tasks. If `counter` were TM-tracked, workers might still be executing after `pthread_join` returns. Fix: caller threads should call `tm_wait_prev_tx()` after their enqueue loops. |
