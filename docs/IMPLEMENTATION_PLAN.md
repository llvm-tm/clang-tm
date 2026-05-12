# LLVM TM Plugin — Implementation Plan

## Status: PARTIALLY DONE

**Completed:**
- `src/tm_method_instrumentation.hpp` — created (346 lines)

**Remaining:**
- Modify `src/TMInstrumentPass.cpp` with all 4 fixes
- Build and test
- Update `examples/` files

---

## Remaining Work

### 1. Fix 4: Preserve return values
**File:** `TMInstrumentPass.cpp`

Add after line ~327 (after `JmpRetGV` creation):
```cpp
GlobalVariable *TxRetValGV = M->getGlobalVariable("tm_tx_return_value");
if (!TxRetValGV) {
    TxRetValGV = new GlobalVariable(*M, F.getReturnType(), false,
                                     GlobalValue::ExternalLinkage, nullptr,
                                     "tm_tx_return_value");
    TxRetValGV->setThreadLocal(true);
}
```

In the returns loop (around line 541), after `splitBasicBlock` and `eraseFromParent`, insert store to thread-local at the start of NewBB before the counter check.

In `OuterEndBB` (around line 529), after `tm_end()` + `JmpRetGV=0`, load from `tm_tx_return_value`.

In `CleanupBB` (around line 578), return the loaded value instead of null.

### 2. Fix 1: Method call instrumentation
**File:** `TMInstrumentPass.cpp` — add `#include "tm_method_instrumentation.hpp"`
In `TMGlobalInitPass::run()`, after symbol table creation (~line 149), call:
```cpp
tm_method_instrumentation::processMethodCalls(M, tm_read_i1, ..., tm_write_ptr);
```

### 3. Fix 3: Memory intrinsic instrumentation
**File:** `TMInstrumentPass.cpp` — in the instruction iteration loop (around line 424), add:
- Detect `CallInst` targeting `llvm.memcpy`, `llvm.memmove`, `llvm.memset`
- Check if any operand traces to a TM global
- Replace with per-byte loops using individual `tm_read_i1`/`tm_write_i1` calls

### 4. Fix 2: Explicit pthread/thread detection
**File:** `TMInstrumentPass.cpp` and new `src/tm_thread_symbols.hpp`

Create `tm_thread_symbols.hpp` with a configurable global list:
```cpp
static const char *const ThreadEntrySymbols[] = {
    "pthread_create",
    "_ZNSt3__16threadC1Em",   // std::thread constructor
    // Future additions go here
};
```

In `TMGlobalInitPass`, replace the heuristic (hasTM || callsTx) with:
1. Detect thread entry points via symbol list (explicit)
2. Mark their transitive call closure as thread entry points
3. If no explicit matches found, fall back to heuristic

---

## File Changes Summary

| File | Change |
|------|--------|
| `src/TMInstrumentPass.cpp` | Fixes 1-4 (main implementation) |
| `src/tm_method_instrumentation.hpp` | DONE — method cloning helpers |
| `src/tm_thread_symbols.hpp` | NEW — configurable thread entry symbol list |
| `examples/types_instrumented.cpp` | Update for Fix 4 (return value) |
| `examples/nested_instrumented.cpp` | Update for Fix 4 |
| `examples/threads_instrumented.cpp` | Update for Fix 4 |
| `examples/memtest_instrumented.cpp` | Update for Fix 4 + Fix 3 |
| `examples/retry_instrumented.cpp` | Update for Fix 4 |

---

## Build & Test Commands

```bash
cd /Users/daniel/Projects/TM/tm_api_cpp/llvm_tm_plugin
make clean && make -j4
make test && make run
```