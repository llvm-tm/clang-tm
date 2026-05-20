# STMbench7 -O1 Crash Debug TODO

## Crash Signature

- **Binary**: `stmbench_tinystm_wbctl` (TinySTM write-back CTL backend)
- **Workload**: op_sm1_create_cp (workload 1, 90% read / 10% write)
- **Crash**: `read_word_ctl: addr=0x10 sz=7` — null pointer deref at offset 0x10
- **Reproducible**: Yes, with `-t 1` (1 thread) — sequential bug, not a concurrency issue
- **Frequency**: 100% deterministic, always at the same code location

## Pipeline

The TM instrumentation pipeline used is (from `tm_pipeline.mk`):

1. **Compile**: `clang++ -O1 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -fno-stack-protector -pthread -emit-llvm -c` → `.bc`
2. **Instrument**: `opt -load-pass-plugin=libTMInstrument.so -passes="tm-instrument"` → `.instr.bc`
3. **Optimize**: `opt -O0` → `.merged.opt.bc` (merged with runtime, compiled at -O1)
4. **Link**: `clang++ -O0 -pthread` → binary

**Key observation**: Both O0 and O1 fail to replace `_Znwm`/`_Zna` with `tm_malloc` in the instrument pass (0 `tm_malloc` calls in either output), yet O0 works fine. So this is NOT the root cause.

## Backtrace

```
0  read_word_ctl + 280        tm_read_ptr → crash at addr=0x10
1  tm_read_ptr + 124
2  $_2::__invoke + 1460       (actually __tree_balance_after_insert_tm_clone, private function)
3  op_sm1_create_cp + 5852    (call at offset 0x16DC = 0x100006C28)
4  wrap_sm1 + 488
5  worker + 1476
```

The crash address is always `0x10` = offset of `__parent_` field in `__tree_node_base`:
```
offset 0: __left_    (pointer)
offset 8: __right_   (pointer)
offset 16: __parent_ (pointer)  ← crash at 0x10 = null + 16
offset 24: __is_black_ (bool)
offset 28: key/value data
```

## Key Difference: O0 vs O1

| Aspect | O0 (works) | O1 (crashes) |
|--------|-----------|--------------|
| `_Znwm` form | `call` → replaced with `tm_malloc` | `invoke` → NOT replaced (pass only checks `CallInst`) |
| Link-time override | Works fine | Works fine (tm_alloc_overrides.hpp) |
| `__tree_balance_after_insert` | Not inlined in op_sm1_create_cp | Inlined, then tm_clone'd |
| Calls to clone in function | 0 | 8 (pairs with 8 `_Znwm` calls) |
| Stack frame | 0x? | 0x4c0 bytes |

## Disassembly Findings

### `_Znwm` → stack slot (op_sm1_create_cp + 0x1470):
```
1000069c0: bl    __Znwm                    // x0 = operator new(40)
1000069c4: sub   x8, x29, #0x338           // x8 = x29 - 0x338
1000069c8: stur  x0, [x8, #-0x100]         // [x29-0x438] = x0 (%100, non-null)
```

### Stack slot → `__tree_balance_after_insert_tm_clone` call (op_sm1_create_cp + 0x16D8):
```
100006c20: sub   x8, x29, #0x338           // x8 = x29 - 0x338
100006c24: ldur  x1, [x8, #-0x100]         // x1 = [x29-0x438]
100006c28: bl    0x100016d90               // __tree_balance_after_insert_tm_clone(x0, x1)
```

### Frame layout
- Frame size: 0x4c0 (from `sub sp, sp, #0x4c0` at function entry)
- x29 = sp + 0x4b0 (from `add x29, sp, #0x4b0`)
- Slot [x29-0x438] = sp + (0x4b0 - 0x438) = sp + 0x78 — WITHIN the frame, safe from callee overwrites

### Validation: no stack slot clobbering
Between the store at 0x1000069c8 and the load at 0x100006c24, there are:
- 18 `stur` instructions to various x29 offsets (0x478, 0x470, 0x468, 0x460, 0x458, 0x488, 0x498, 0x4a0, 0x4b0, 0x4b8, 0x4c0, 0x4a8, 0x4f0)
- **NONE use x29-0x338 as base register to write to x29-0x438**
- Function calls (tm_write_ptr, tm_read_ptr, tm_memset) use their OWN frames below sp — cannot reach [x29-0x438] = sp+0x78

**Conclusion**: The value at [x29-0x438] is correctly stored and never overwritten. The argument to __tree_balance_after_insert_tm_clone SHOULD be non-null.

## EH Landing Pads

The O1 instrumented IR has multiple `invoke` instructions with cleanup landing pads in `op_sm1_create_cp`:
- `%239`: `cleanup` landing pad (unwind for `%100 = invoke @_Znwm(40)`)
- `%237`, `%241`, `%379`: other cleanup pads

These contain TM rollback operations. Since `_Znwm` normally doesn't throw, these paths are likely never taken.

## Unresolved Questions

1. **How does the second argument become null at runtime?**
   - Stack slot analysis shows no clobbering
   - Value should be the non-null `_Znwm` return
   - No other writes to this slot between store and load

2. **Is 0x100016d90 really `__tree_balance_after_insert_tm_clone`?**
   - The cloned function is `private` (no external symbol)
   - 0x100016d90 is AFTER `$_2::__invoke` thunk (0x1000169e4) and BEFORE `is_locked_by` (0x100018bdc)
   - The code at that address matches `__tree_balance_after_insert` body
   - But the crash at offset 0x208 (inside the clone) reads NULL+0x10

3. **Could there be a function merging bug?**
   - The clone has `local_unnamed_addr` → linker could merge identical functions
   - Are there multiple copies of `__tree_balance_after_insert_tm_clone`?

4. **Could the `__tree_balance_after_insert_tm_clone` be reading wrong argument register?**
   - x1 should be second arg, but what if function reads from x0?

5. **Cleanup landing pad effect on regular control flow?**
   - Do the EH cleanup paths have side effects on the normal path?

## Next Steps / Things to Try

1. Add `InvokeInst` handling to the instrument pass (malloc/free/_Znwm replacement) — even though it's probably not the root cause, it's a correctness fix
2. Build with `-O0` compile flags and explicit `-always-inline` to get STL inlining without `invoke`
3. Check if the `__tree_balance_after_insert_tm_clone` clone is called directly vs. through the original function
4. Disassemble the clone function at 0x100016d90 to verify argument mapping
5. Add `printf`/logging before the crash to dump the argument values
6. Check if `is_locked_by` at 0x100018bdc is the correct symbol boundary
7. Try `-fno-exceptions` to prevent `invoke` generation
