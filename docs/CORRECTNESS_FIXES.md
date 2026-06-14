# Correctness Fix Plan — Implementation Status

| # | Issue | Status | Commit |
|---|-------|--------|--------|
| 1 | SwissTM debug fprintfs (performance) | **Fixed** | `ed1c75e` |
| 2 | Debug patch scripts path fix | **Fixed** | `27936d4` |
| 3 | NOrec STMbench7/TPC-C crash (SIGSEGV) | **Fixed** | `e885078` |
| 4 | LEFTRIGHT multi-thread deadlock | **Fixed** | `1de1b90` |
| 5 | XTM rbtree segfault | **Fixed** | `62d3878` |

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

## Known remaining issues (not yet fixed)

| Issue | Severity | Notes |
|-------|----------|-------|
| TinySTM STMbench7 crash (STL vector realloc) | High | STL-in-TM incompatibility, pre-existing |
| LEFTRIGHT write-set `observed_version` | Low | Uses global clock at read time instead of per-address version; correct with global lock but more conservative than needed |
| stmbench7 times out with >1 thread | Low | Data race in `ts_multimap::lower_bound()` — pre-existing |
