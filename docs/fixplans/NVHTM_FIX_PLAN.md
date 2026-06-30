# NVHTM Fix Plan

## Discovery: The Real Bugs

Three independent bugs were found during code review. Two are correctness-critical;
the third is a design-reality mismatch.

### Bug 1 (CRITICAL): Dead-code `return` in `tm_write()` — nvhtm.hpp:248

```cpp
if (!addr || (uintptr_t)addr < 0x100000 || ((uintptr_t)addr >> 47) != 0)
//    ↑ no braces — body is only the preprocessor block

#ifdef LLVM_TM_PLUGIN
    if (!stm::isTMAddress(addr)) { *addr = val; return; }
#else
    TM_ASSERT(...);
#endif
    return;    // ← line 248: OUTSIDE the `if` — always executes
```

The `#ifdef`/`#else`/`#endif` block is the single-statement body of the outer `if`.
The `return;` at line 248 comes AFTER the `if` body and **always executes**
when control reaches it.

**Effect**: Whenever the address passes the null/kernel-space guard (i.e. every
valid TM address), `tm_write()` returns immediately at line 248 without
logging the write AND without writing through to `*addr`.  The entire write
is silently dropped.

In HTM mode (`_xbegin` succeeded, `tx->active == true`):
- Redo log is NEVER populated → `durable_commit()` always returns at
  `log_count == 0` → zero NVM persistence
- Write-through on lines 263, 279 is NEVER reached → the written value
  never reaches the shared memory location
- HTM transaction succeeds (no conflicting reads), the caller's `counter += 1`
  does nothing observable

**Writes are silently lost on all HTM-capable hardware.**  This is the root
cause of any correctness failure when using the NVHTM backend.

### Bug 2: `_mm_clflush` flushes wrong address — nvhtm.hpp:186

`durable_commit()` line 186:
```cpp
_mm_clflush(&tx->log[i]);   // flushes log-entry cache line
```

`tx->log[]` lives in the `Transaction` struct which is heap-allocated
via `new Transaction()` — this is DRAM (volatile).  Flushing a DRAM
address to NVM pressure does nothing for crash consistency.  The instruction
that should be flushed is the **final address** after writing the value:

```
write_value_to_addr(tx->log[i].addr, ...)   // write to NVM final addr
_mm_clflush(tx->log[i].addr)                 // flush that addr to NVM
```

**Effect**: Even if the redo log were populated, the `_mm_clflush` would
flush the log entry (DRAM) instead of the written value (NVM addr).
A crash between `_xend()` and the NVM-media visibility of the written
values would lose the last committed transaction.

### Bug 3: DRAM reality vs NVM pretense

The TM region is `mmap(MAP_ANONYMOUS)` — pure volatile DRAM, not NVM.
The `MAP_SYNC` or DAX-backed file needed for real NVM persistence is not
present.  This means:

- **No NVM recovery is possible** — DRAM vanishes on crash
- **`_mm_clflush` is a no-op** — DRAM doesn't need flushing
- **Checkpoint + recovery from Implementation_notes.md was never implemented**
  and would serve no purpose on DRAM

The write-through + log pattern for NVM durability was designed as if the
TM region were NVM-backed, but the actual memory allocator uses anonymous
DRAM.  This is not a bug per se — the code works correctly for HTM
atomicity on DRAM — but the NVM pretense is misleading and the
`_mm_clflush` instructions waste cycles.

### Root-cause chain summary

```
Bug 1 (dead return) → redo log empty → durable_commit no-ops →
  writes silently lost in HTM mode

Bug 2 (wrong clflush target) → even with Bug 1 fixed, crashes
  after _xend() could lose writes on real NVM

Bug 3 (DRAM pretense) → even with both bugs fixed, checkpoint/
  recovery infrastructure is missing for true NVM support
```

## Fix Plan

### Fix 1: Restructure `tm_write()` — correct the braces

Add braces around the null-address-guard preprocessor block so the
`return` is its body, not a follow-on statement.  The guard:

```cpp
if (!addr || (uintptr_t)addr < 0x100000 || ((uintptr_t)addr >> 47) != 0) {
#ifdef LLVM_TM_PLUGIN
    if (!stm::isTMAddress(addr)) { *addr = val; return; }
#else
    TM_ASSERT(stm::isTMAddress(addr), "…");
#endif
    return;
}
```

After this change, valid TM addresses fall through to the redo-log +
write-through path (lines 250-280), restoring both in-TX logging and
HTM-tracked writes.

### Fix 2: Restructure `durable_commit()` — flush final addresses

Change from flushing `&tx->log[i]` to flushing the final address
after writing the value:

```cpp
for (size_t i = 0; i < tx->log_count; i++) {
    write_value_to_addr(tx->log[i].addr, tx->log[i].new_val, tx->log[i].type);
    _mm_clflush(tx->log[i].addr);
}
_mm_sfence();
```

On DRAM this is a no-op (harmless).  On real NVM it provides the
correct persist ordering: write then flush the written address.

### Fix 3: Document DRAM constraint accurately

Update `Implementation_notes.md`:
- State clearly that the TM region is `mmap(MAP_ANONYMOUS)` DRAM
- Mark `durable_commit()` as an NVM-readiness scaffold, not active
- Remove checkpoint+recovery prose that was never implemented
- Add a note: "To use with NVM, replace `MAP_ANONYMOUS` with
  `MAP_SYNC` on a DAX filesystem; then checkpoint and recovery
  must be implemented"

### Fix 4 (stretch): Add checkpoint marker to TM region

For future NVM support, allocate a 64-byte checkpoint slot at
`g_tm_region_start`.  `durable_commit()` writes a magic cookie
before applying writes and clears it after.  `tm_region_init()`
checks for an uncleared cookie and replays the redo log.  This
is a no-functional-change on DRAM.

## Verification

| Test | Expected | Notes |
|------|----------|-------|
| `tm_write` inside active HTM TX | Value reaches `*addr` | HTM write-set tracks it |
| `durable_commit` after `_xend` | Log-written values applied to final addrs | On DRAM: immediate visibility |
| `_mm_clflush` during `durable_commit` | Flushes the FINAL addr, not the log entry | On DRAM: no-op |
| Re-init with uncleared checkpoint | No action (DRAM: checkpoint always zeroed) | Future NVM: replay |
| Multi-thread correctness | HTM conflict detection provides atomicity | As before, no change |

## Unchanged

- **Write-through inside HTM**: Correct for DRAM-backed HTM.  HTM tracks
  L1 write-set; on abort, L1 is invalidated; on DRAM, stale values in
  the memory hierarchy are invisible (next read fetches from DRAM which
  has the old value via invalidation).  On NVM this would require
  careful handling, but we are DRAM-only now.
- **HTM read path** (`tm_read` returns `*addr`): Correct — HTM tracks
  read-set in hardware.
- **Retry / fallback logic**: Unchanged.
