# Correctness Fix Plan

Three correctness-impacting issues across TM backends.

## 1. NOrec — STMbench7/TPC-C crash (SIGSEGV)

**Root cause:** `read_word_norec` has no address validation. When the LLVM plugin
instruments a null-pointer-derived GEP address (e.g., `&node->right` where `node`
became null due to concurrent mutation), `memcpy` from the near-null address
crashes.

All other backends guard against this with either:
- `isTMAddress()` bypass for valid non-TM heap addresses, or
- `< 0x100000` null-guard for near-null GEP results.

NOrec has neither.

**Fix:** Add both guards to three locations:

1. `read_word_norec` — bypass before NOREC double-check protocol
2. `write_word_norec` — bypass before write-set creation
3. `commit()` write-back loop — skip non-TM write-set entries

Pattern (same as WBCTL):
```
#ifdef LLVM_TM_PLUGIN
if (!stm::isTMAddress(addr)) {
    if (addr == nullptr || (uintptr_t)addr < 0x100000) return zero;
    return read_value_from_addr(addr, sz);
}
#endif
```

## 2. LEFTRIGHT — Multi-thread deadlock (bank/ycsb)

**Root cause:** Three bugs in the left-right barrier commit protocol:

1. **Deadlock:** Read-only transactions skip both barriers entirely, but
   `thr_counter` expects all threads to participate. Writer threads spin
   forever waiting for read-only threads to enter the barrier.

2. **Write-write conflict:** Right barrier only counts entries, does not
   serialize write-back — concurrent writers race on the same address.

3. **Weak validation:** `validate()` checks a stale version condition, not
   the current address state.

**Fix (Option B):** Replace the broken left-right barrier protocol with a
global-commit-lock protocol (similar to NOrec's write path):

- Remove `g_left_barrier`, `g_right_barrier`, `g_left_phase`, `g_right_phase`
- Add `g_commit_lock` (atomic int)
- Commit: validate → acquire lock → re-validate → write-back → release lock
- Lock released before `abort_tx()`/`siglongjmp` to avoid deadlock

## 3. XTM — rbtree segfault

**Root cause:** XTM's full-page (4096 bytes) `memcpy` write-back in commit
overwrites the `ChunkHeader` and allocator bitmap that reside on the same
4 KB page as data blocks. This corrupts allocator metadata, causing
double-free / bad pointer dereference.

**Fix:** Isolate allocator metadata from data pages by skipping to the next
page boundary before placing data blocks in each 64 KB chunk:

```
// In tm_region_init for each size class:
uint32_t data_off = (hdr_total + 4095) & ~4095;
```
