# Session Summary

## Goal
Maintain the LLVM IR-level TM plugin pipeline and fix benchmark failures for LLVM 22. Current focus: opaque function resolution tool + pipeline integration + test infrastructure.

## Key Changes This Session

### 1. Plugin: Always scan for opaque functions (TMInstrumentPass.cpp:129-139)
`checkOpaqueOrAbort` now always calls `checkOpaqueFunctions()` when there are reachable functions, so `-tm-opaque-symbols-file=<path>` works even with `-tm-allow-opaque`. Previously the scan was skipped entirely when opaque functions were allowed.

### 2. New: `tm-resolve-opaque.py` (llvm_tm_plugin/)
Script to resolve opaque function symbols against system libraries.
- Reads symbol names (one per line) from a file
- Resolves known math/libc/pthread functions from built-in signature table (fast path)
- Searches targeted system libraries (libm.so, libc.so, etc.) via `nm -D --defined-only`
- Generates LLVM IR stub declarations and assembles them to bitcode via `llvm-as`
- Reports unresolved symbols with suggestions

### 3. New: `test/test_math_opaque.cpp`
Test file using `sqrt`, `cos`, `sin`, `pow`, `log` inside a `TX` function. Uses runtime variables (`a`, `b`, `c`) to prevent constant folding.

### 4. Pipeline integration in tm_pipeline.mk
- `TM_OPAQUE_SYMBOLS_FILE` variable — passed to plugin when set
- `tm_resolve_opaque` canned recipe — runs resolve tool on symbol file
- `TM_OPAQUE_STUBS` — auto-linked into merged bitcode if present
- `TM_LINK_LIBS` — passes `-lm` etc. to final link step
- Documentation updated in header comment

### 5. InvokeInst handling + TMTracedArgs for clones (THIS SESSION)
All 14 plugin tests pass (12 standard + 2 race tests). Bank benchmark confirms money conservation.

- `tm_local_vars.hpp`: `isHeapAllocationCall`, `isDeallocationCall`, `tracesFromTMGlobal`
  changed from `CallInst` to `CallBase` — handles `InvokeInst` that LLVM emits at -O1+ (e.g. `_Znwm`)
- `tm_instrument_helpers.hpp`: `handleMallocFree`, `handleMemoryIntrinsic` accept `CallBase*`;
  `InvokeInst` is erased immediately and replaced with `CallInst` + `BranchInst` to fix CFG
- `TMInstrumentPass.cpp`: both inline and legacy pipeline loops updated to use `CallBase`
- `tm_method_instrumentation.hpp`: new pass 1.5 in `cloneTxReachableGraph` copies `TMTracedArgs`
  from originals to clones so `isTMTracedPtr` works in cloned function bodies

## Key Files
- `llvm_tm_plugin/src/TMInstrumentPass.cpp` — InvokeInst fix + checkOpaqueOrAbort fix
- `llvm_tm_plugin/src/opaque_safe_table.hpp` — shared known-safe table (+ `emitOpaqueSuggestion`)
- `llvm_tm_plugin/tm-resolve-opaque.py` — NEW: resolve tool
- `llvm_tm_plugin/tm_pipeline.mk` — opaque resolution integration
- `llvm_tm_plugin/test/test_math_opaque.cpp` — NEW: math opaque test
- `llvm_tm_plugin/src/tm_local_vars.hpp` — CallBase change
- `llvm_tm_plugin/src/tm_instrument_helpers.hpp` — CallBase* + InvokeInst fixup
- `llvm_tm_plugin/src/tm_method_instrumentation.hpp` — TMTracedArgs propagation

### 6. wbctl read_set cache fix: STMbench7 at 1t (THIS SESSION)
Root cause: the `read_word_ctl` in `tinystm_wbctl.hpp` cached values in the read_set and returned the cached value on subsequent reads of the same address. Stack addresses used as iterator slot storage (e.g., `begin()` pointer in range-for loops) were modified by direct stores (++it), but the read_set returned the stale cached value. This caused the loop to iterate past `end()`, reading uninitialized memory and computing a garbage `conn_idx` → out-of-bounds `g_connections` access → SEGV.

Fix: in `read_word_ctl` (`tinystm_wbctl.hpp:294-300`), when a read_set hit is found, still re-read from memory instead of returning the cached value. The version validation is kept for commit-time correctness. This is the only backend with this caching pattern — wbetl, wt, TL2, NOrec, and SwissTM all always read fresh.

Valgrind confirmed: 0 errors (previously 10M+). All 14 plugin tests pass.

**Pre-existing**: multi-threaded (4+ threads) crashes in `_M_insert_unique_node` — pre-existing, not related to this fix.

## Known Issues
- Labyrinth: TM annotation on `g_labyrinth` fixed pointer analysis (do_expansion/do_traceback now cloned and instrumented).
  Verified clean at all thread counts (1-16t, 0 aborts, verification passed).
- The earlier "crash at 2 threads" was caused by stale binary from partial file reverts during investigation.
  Clean rebuild works perfectly.
- STMbench7: 1 thread wbctl FIXED (read_set cache stale-value bug).
  Multi-threaded (4+ threads) crashes: root cause identified.

## Root Cause of Multi-threaded Crashes (THIS SESSION)

### Missing instrumentation of `std::vector::push_back` internal stores

The STMbench7 multi-threaded crash is a **use-after-free** caused by `std::vector::push_back` (and related operations like `emplace_back`, `__split_buffer`) making **direct stores** to vector internals (`__begin_`, `__end_`, `__end_cap_`) that bypass TM barriers.

**Why stores are missed** (the `tm-instrument` legacy pipeline):
1. Pipeline uses `-O1 -fno-inline` — vector internals are NOT inlined into TX function bodies
2. `cloneTxReachableGraph` creates `_tm_clone` versions of vector internals, but clones are **instrumented BEFORE call redirection** (Pass 1 of `cloneTxReachableGraph`)
3. At instrumentation time, clones have **zero callers** — `tracesFromTMGlobal` traces `this` → load from alloca → store of argument (to redirect target) → checks callers → **none** → returns false
4. `isSharedPointer` falls through → returns false → store is NOT replaced with `tm_write_ptr`
5. Original `push_back`'s direct stores to `__begin_`, `__end_` operate on unconrolled (non-TM-tracked) memory
6. Concurrent reads in the range-for loop (`op_st3_traverse`) via `tm_read_ptr` get stale copies, then walk into **freed memory** when push_back reallocates the vector buffer

**Fix applied** (`tm_pipeline.mk:140`):
- Switched default pipeline from `tm-instrument` to `tm-instrument-inline`
- The inline pipeline: `TMInitInjectPass` (clone with `alwaysinline` + redirect callers) → `AlwaysInlinerPass` (inline clones into TX body) → `TMInstrumentInlinePass` (instrument TX functions)
- After inlining, stores directly GEP from TM globals → `tracesFromTMGlobal` traces → stores are correctly instrumented
- The `tm-instrument-inline` pipeline was already implemented but not the default

### 7. Fix `tm-instrument` legacy pipeline: instrument-after-redirect (THIS SESSION)
The no-inline `tm-instrument` pipeline previously instrumented clones in `cloneMethod` (Pass 1 of `cloneTxReachableGraph`), **before** call redirection (Pass 2). At that point clones had zero callers, so `tracesFromTMGlobal` for argument values (`this` → alloca → store of arg) checked callers → none → returned false → `originatesFromLocal` found the alloca → returned false → stores skipped.

**Fix** (`tm_method_instrumentation.hpp`, `TMInstrumentPass.cpp`):
- Added `CloneMode::CloneOnly` — clones without instrumentation
- `TMGlobalInitPass` now: `cloneTxReachableGraph(CloneOnly)` → `redirectTXFunctionsToClones` → `instrumentAllClones(ClonedMap)`
- New `instrumentAllClones()` iterates all cloned functions post-redirect, propagates TMTracedArgs, adds `NoInline` attribute, and calls `instrumentLoadsStoresInFunction`
- After redirect, `tracesFromTMGlobal` finds actual callers and traces `this` → GEP from TM global → correctly instruments stores to vector internals

**Key files**: `tm_method_instrumentation.hpp:38` (CloneOnly enum), `:259-276` (CloneOnly branch), `:489-503` (instrumentAllClones), `TMInstrumentPass.cpp:250-257` (restructured TMGlobalInitPass)

### 8. Fix `redirectCallsToClones` + `noinline` stripping for AlwaysInline mode (THIS SESSION)
Two bugs in the `tm-instrument-inline` pipeline prevented proper inlining of `_ConstructTransactionD2`:

**`redirectCallsToClones`**: In AlwaysInline mode, the redirect skipped calls whose callee had no TM-traced arguments (`hasTMArg` check). This left calls to `_ConstructTransactionD2` (which stores `__end_` back to the vector) pointing to the original, not the clone. The D2 clone was never inlined → its store bypassed TM barriers.

**`noinline` stripping**: The `strip-alwaysinline-for-noinline` logic in `TMInstrumentPass.cpp` was stripping `alwaysinline` instead of `noinline` from clone attributes, preventing the `AlwaysInlinerPass` from inlining them.

**Fixes**:
- `tm_method_instrumentation.hpp`: `redirectCallsToClones` now ignores `hasTMArg` in AlwaysInline mode — redirects ALL calls unconditionally
- `TMInstrumentPass.cpp`: strip `noinline` instead of `alwaysinline` from cloned functions' attributes
- `tm_runtime.cpp`: add `tm_write_ptr` and `tm_write_word` barrier wrappers for generic pointer types; add `free` size awareness in `tm_free`
- `NOrec_runtime.cpp`, `SwissTM_runtime.cpp`, `TinySTM_runtime.cpp`, `tl2_runtime.cpp`: add missing `tm_read_ptr` and `tm_write_ptr` exports
- `tinystm_wbctl.hpp`: handle NULL address in `read_word_ctl` to avoid SEGV during instrumentation of partially-initialized writes
- `tm_alloc_overrides.hpp`: new file — intercepts `malloc`/`free`/`new`/`delete` in TM tests to avoid using system allocator (which may deadlock under concurrent TM); uses `tm_malloc`/`tm_free` for allocations in instrumented code

### 9. `vector_realloc_test` still crashes with `tm-instrument-inline` + WBCTL (INVESTIGATING)
Even after the above fixes, the concurrent reader+writer `vector_realloc_test` crashes with `NULL_WRITE` at `tx=3, ws=38` under WBCTL backend.

**What was checked**:
- D2 (`_ConstructTransactionD2`) IS now cloned and inlined (D2_tm_clone created, then inlined into D1_tm_clone, then into `push_back_tm_clone`)
- The store to `g_vec.__end_` in the fast path IS instrumented: `call void @tm_write_ptr(ptr getelementptr inbounds nuw (i8, ptr @g_vec, i64 8), ptr %389)` at instrumented IR line 668
- Store inside `__split_buffer.clear()` (realloc path) writes to stack-allocated `__split_buffer` fields — not instrumented, but these are stack locals, not TM globals
- Original D2 function definition still present (not instrumented) — used by the original (non-cloned) D1 in uninstrumented code paths

**Hypothesis for remaining crash**: Non-instrumented stores to `__split_buffer` internals (or some other function not cloned/inlined) corrupt data that the instrumented fast path reads. Root cause still unknown — need to identify WHICH uninstrumented store causes the corruption.

## Next Steps
1. Identify the specific uninstrumented store in the `tm-instrument-inline` pipeline that corrupts vector state during concurrent access
2. Verify the `tm-instrument` (no-inline) pipeline with the instrument-after-redirect fix
3. Extend `guess_declaration_ir()` with more function signatures as needed
4. Run full benchmark suite to verify no regressions
5. Test STMbench7 (3 workloads) + TPC-C at 16 threads
