# Bug Assessment

## Bug 1: Retry Logic in Plugin (Critical)

**File**: `llvm_tm_plugin/src/TMInstrumentPass.cpp:453`
**Status**: Present — `(counter==0 OR jmpret!=0)` condition
**Impact**: Transaction abort + retry causes assertion failure in TinySTM

**Root cause**: The outer/nested decision in every `@transaction` function uses `jmpret != 0` as an additional condition for the outer path. After a longjmp abort, `jmpret` is non-zero (set by sigsetjmp's return value). This is correct for the top-level function that was aborted, but ALL nested `@transaction` functions also see `jmpret != 0`, causing them to take the outer path (calling `tm_begin()`) when the transaction is already active.

**Trace**:
1. Top-level TX: counter=0, jmpret=0 → Outer → sigsetjmp → tm_begin() → jmpret=0 → cont
2. Nested TX: counter=1, jmpret=0 → Nested → counter=2 → work → counter=1 → ret
3. TinySTM detects conflict, calls longjmp back to top-level sigsetjmp
4. Top-level (retry): counter=0, jmpret=1 → Outer → sigsetjmp → store 1 to jmpret → tm_begin() ✓
5. Nested (retry): counter=1, jmpret=1 → `(1==0 OR 1!=0)` = TRUE → Outer → sigsetjmp → tm_begin() → **CRASH** (tx->active is already true)

**Fix**: After the outer path calls `tm_begin()` successfully, clear `jmpret` to 0. Nested functions then see `jmpret=0`, `counter>0`, correctly taking the nested path.

---

## Bug 2: Return Values Discarded (Medium)

**File**: `llvm_tm_plugin/src/TMInstrumentPass.cpp:693-699`
**Status**: Present — returns `undef`/`null` for non-void functions
**Impact**: Any `@transaction` function returning a value gets silent data corruption

**Root cause**: The CleanupBB ignores original return values and always returns void/null:

```cpp
if (F.getReturnType()->isVoidTy()) {
    CleanupBuilder.CreateRetVoid();
} else {
    CleanupBuilder.CreateRet(Constant::getNullValue(F.getReturnType()));
}
```

**Fix**: Before erasing ReturnInsts, thread each non-void return value through to CleanupBB via a store/load pattern with a thread-local temporary. The CleanupBB loads the stored value and returns it.

---

## Bug 3: Memory Intrinsics Deleted (Medium)

**File**: `llvm_tm_plugin/src/TMInstrumentPass.cpp:534-539`
**Status**: Present — `llvm.memcpy`/`memmove`/`memset` are simply erased with TODO
**Impact**: Programs using `memcpy`/`memset` on TM-annotated data get silent corruption

**Root cause**: The plugin detects memory intrinsics that touch TM globals but only marks them for erasure instead of replacing with per-byte instrumented loops.

**Fix**: Replace each erased intrinsic with a loop that calls `tm_read_i1`/`tm_write_i1` per byte. For `memset`, use `tm_write_i1` in a loop. For `memcpy`/`memmove`, use `tm_read_i1` + `tm_write_i1` per byte.

---

## Bug 4: Wrong TL2 Filename (Low)

**File**: `benchmarks/test/bank/Makefile:73`
**Status**: Present — references `TL2_runtime.cpp` (capital T), file is `tl2_runtime.cpp`
**Impact**: `make bank_tl2` fails with missing file error

**Fix**: Change `TL2_runtime.cpp` → `tl2_runtime.cpp`.

---

## Summary

| Bug | Severity | Fix Complexity | File |
|-----|----------|---------------|------|
| 1. Retry logic | Critical | 1 line | TMInstrumentPass.cpp:478-479 |
| 2. Return values | Medium | ~20 lines | TMInstrumentPass.cpp:656-699 |
| 3. Memory intrinsics | Medium | ~40 lines | TMInstrumentPass.cpp:520-541 |
| 4. TL2 filename | Low | 1 char | bank/Makefile:73 |
