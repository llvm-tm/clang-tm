# Bug Assessment: `_tm_clone` Approach

## 1. Summary

Three issues were found during `std::set<int>` testing with the `parent_set_race` stress test. The root cause in all cases traces to the `_tm_clone` mechanism: cloned STL functions interact incorrectly with LLVM's optimizer at `-O3`, producing broken IR.

### Bug A: `std::get<1>` optimization mismatch (FIXED, May 17 2026)

**Root cause:** `std::get<1>` (i.e. `__get_pair<1>::get`) is marked `noinline` in libc++. When a `_tm_clone` function calls it and later passes the result to `tm_read_ptr`, the `-O3` pass transforms `get(pair)` from "return `&pair.second`" to "return `pair.second`" (bypassing the pointer), which leaves callers doing `load ptr, ptr %ptr_from_get` — a load from a non-pointer value or garbage.

**Original fix (`replaceGetCalls`):** Pattern-match mangled `std::get<0>`/`std::get<1>` calls in clones and replace with direct `GEP` instructions. Removed in favor of the permanent fix below.

**Permanent fix:** Strip `noinline` (and `optnone`, `minsize`, `naked`) from ALL cloned functions in `cloneMethodWithSuffix()`. This lets `-O3` inline cloned `std::get<N>` naturally via GEP, eliminating the mismatch at its source. `replaceGetCalls()` was deleted.

### Bug B: TinySTM crash at `g_set + 8` (FIXED, May 17 2026)

**Symptom:** deterministic `EXC_BAD_ACCESS` with program counter = `g_set + 8` (data section), exit code 138. SGL backend worked fine at 400K elements.

**Root cause:** Two interacting problems:
1. `computeClonableFunctions()` used a complex argument-traceability fixed-point algorithm that could miss functions that need cloning (documented fragile corners #1-5). When a function was NOT cloned but SHOULD have been, its uninstrumented loads/stores executed inside a transaction, corrupting TM state.
2. `cloneMethodWithSuffix()` explicitly added `NoInline` to ALL clones. This prevented `-O3` from inlining cloned functions, exposing the optimizer to the `std::get<N>` pattern mismatch from Bug A.

**Fix:**
- Simplified `computeClonableFunctions()`: clone ALL TX-reachable non-TX functions (no more argument-traceability analysis). Deleted the old fixed-point algorithm.
- Stripped `noinline`, `optnone`, `minsize`, `naked` from cloned functions.

**Verification:** 5/5 TinySTM runs pass at 400K elements (200K per thread, 4 threads).

### Bug C: Opaque function calls inside transactions (DETECTED, May 17 2026)

**Problem:** Functions from external libraries (declarations without a body in the current TU) cannot be cloned or instrumented. If called from inside a TX, their memory accesses bypass TM instrumentation.

**Fix:** Added opaque function detection in `TMGlobalInitPass`. Calls to uninstrumentable functions inside TX-reachable code trigger a compilation error. Suppressible via `-DTM_ALLOW_OPAQUE` or `clang-tm --allow-opaque`.

Known-safe functions are always exempt: `tm_*`, LLVM intrinsics, `sigsetjmp`, `malloc`/`free`, `operator new`/`delete`, `__cxa_*` exception handling, and `_ZSt` standard library helpers.

---

## 2. Fundamental Fragility (Before Fixes)

The original cloning mechanism in `tm_method_instrumentation.hpp` had several structural weaknesses, all now resolved:

### 2.1 Complex, Brittle Clonability Analysis (DELETED)

`computeClonableFunctions()` used a 2-step fixed-point algorithm with 5 known "fragile corners" (documented in the source before deletion):

1. **Shallow Step-3 filter:** Only checks direct loads/stores, not transitive callees.
2. **Depth limit (10 hops):** Long GEP/Load/Call chains lose traceability.
3. **Indirect calls invisible:** Virtual methods and function pointers are ignored.
4. **Argument-only propagation:** Functions that load a TM global directly (not via a parameter) escape seeding if read-only.
5. **Write-only seeding:** Read-only functions are never seeded — fine for commits but fragile if semantics change.

**Replaced with** a simple set-copy: all TX-reachable non-TX functions are cloned. The old analysis code and its documentation were deleted.

### 2.2 Untamed Function Attributes (FIXED)

`cloneMethodWithSuffix()` copied ALL function attributes from the original. Originally, it ALSO explicitly added `NoInline`. This caused:
- `noinline` — prevents inlining that `-O3` would otherwise do correctly
- `writeonly`, `initializes` — corrupting for instrumented code
- `optnone`, `minsize` — prevent optimization of the instrumented version

**Fix:** Strip `noinline`, `optnone`, `minsize`, `naked` from cloned functions. Delete the explicit `NoInline` addition.

### 2.3 Pipeline Ordering (No change needed)

The pipeline `Instrument → -O3` is correct with the fixes. Since clones no longer carry `noinline`, and all TX-reachable functions are cloned, `-O3` can safely inline cloned `std::get<N>` and other helpers.

### 2.4 Runtime Duplicate Definitions (Still open)

The plugin emits thread-local globals (`@tm_jmpbuf`, `@tm_nested_call_counter`, `@tm_longjmp_ret`) into the instrumented bitcode. Each runtime `.cpp` also defines them with `__thread`. This creates duplicate symbol definitions that resolve differently across platforms (Mach-O vs ELF).

**Status:** Not causing current issues but should be cleaned up.

---

## 3. Implementation

### Fix: Simplified `computeClonableFunctions`

Replaced the ~200-line fixed-point algorithm with:
```cpp
static SmallPtrSet<Function *, 32>
computeClonableFunctions(Module &M,
                         SmallPtrSetImpl<Function *> &TxReachableFuncs)
{
    SmallPtrSet<Function *, 32> Clonable;
    for (Function *F : TxReachableFuncs) {
        if (F->isDeclaration()) continue;
        if (F->getName().starts_with("tm_")) continue;
        if (hasAnnotation(*F, "transaction")) continue;
        Clonable.insert(F);
    }
    return Clonable;
}
```

### Fix: Stripped noinline from clones

In `cloneMethodWithSuffix()`, after cloning:
```cpp
NewFunc->removeFnAttr(llvm::Attribute::NoInline);
NewFunc->removeFnAttr(llvm::Attribute::OptimizeNone);
NewFunc->removeFnAttr(llvm::Attribute::MinSize);
NewFunc->removeFnAttr(llvm::Attribute::Naked);
```

### New: Opaque function detection

Added `checkOpaqueFunctions()` in `TMInstrumentPass.cpp`. Called from `TMGlobalInitPass::run()` after the call graph is built. Detects:
1. Calls to function declarations (external/library functions) inside TX-reachable code
2. Indirect calls (virtual methods, function pointers) inside TX-reachable code

---

## 4. Remaining Work

### `-O0` compilation

Tested: the plugin works with `-O0` instead of `-O1 -fno-inline`. However:
- With `-O1 -fno-inline`: 0 clones (STL templates inlined by frontend), 884 lines of IR, SGL passes, TinySTM passes in < 30s
- With `-O0`: 229 clones, 3,266 lines of IR, SGL passes, TinySTM times out (> 120s)

**Conclusion:** `-O1 -fno-inline` is vastly superior. Stick with it.

### Opaque functions

- Plugin emits hard error by default for opaque calls in TX context
- `clang-tm --allow-opaque` suppresses via `-DTM_ALLOW_OPAQUE`
- Per-call suppression: `__attribute__((annotate("tm_allow_opaque")))` on the call
- Indirect calls (virtual methods, function pointers) also detected

### malloc/free

Confirmed: the plugin's job is to replace `malloc`/`free`/`new`/`delete` with `tm_malloc`/`tm_free`/`tm_new`/`tm_delete` in instrumented code. The runtime must provide correct TM-aware allocators.
