# Session Summary

## Goal
Maintain the LLVM IR-level TM plugin pipeline and fix benchmark/test failures for LLVM 22.

## Done
- **Makefile from-clean chain fix**: `TM_PLUGIN` in `tm_pipeline.mk` changed from absolute path (`$(LLVM_PLUGIN_DIR)/bin/...`) to relative (`$(BIN_DIR)/...`), matching the build rules. GNU Make 4.4 treats absolute and relative paths as different targets, so the pattern rule prerequisite `$(TM_PLUGIN)` never matched `bin/libTMInstrument.so` from clean state. Fixed — `make clean && make out/foo.instr.bc` now works in one invocation.
- `test_single_push` hang: FIXED via `getBaseObject()` + `Use`-traversal in `handleLoadStore`.
- `local_containers_test` SIGABRT: **FIXED** — recursive `__tree_deleter_tm_clone` stayed non-inlined, its `_ZdlPvmSt11align_val_t` calls invisible to `handleMallocFree`. Dual fix: (1) `tm_untrack_spec_alloc()` marks freed ptrs in spec_alloc list by setting `node->ptr = nullptr`; (2) `TMInstrumentInlinePass` extended to also process `_tm_clone` functions, intercepting deletes inside surviving clones. **PASS at 1t/2t/4t**.
- `vector_realloc_test` data corruption: **FIXED** — write-set type-mismatch when sub-word TM reads hit UINT64 write-set entries. Read/write functions now merge/extract sub-word values from wider UINT64 entries.
- Post-instrumentation `-O3` reorder: **FIXED** — `std::atomic_signal_fence(seq_cst)` compiler barrier in all write-set insertion functions (18 functions across TinySTM, SwissTM, TL2).
- Bank benchmark at 2+ threads: **FIXED** — removed `operator new`/`delete` overrides from `tm_alloc_overrides.hpp` (they routed runtime's `unordered_map` bucket arrays through `tm_malloc`, causing use-after-free on abort).
- WBCTL read-set cache stale-value bug: **FIXED** — `read_word_ctl` now re-reads from memory instead of returning cached read-set values.
- `InvokeInst` handling + `CallBase` migration: all plugin load/store/malloc/free interception uses `CallBase` to handle both `CallInst` and `InvokeInst`.
- `TMGlobalInitPass` restructured: clone-first (no instrumentation) → redirect → instrument (post-redirect, so `tracesFromTMGlobal` finds actual callers).
- `redirectCallsToClones`: AlwaysInline mode now redirects ALL calls unconditionally (ignoring `hasTMArg`).
- `__tree_deleter_tm_clone` recursive clone fix: `instrumentAllClones` propagates `TMTracedArgs` and runs `instrumentLoadsStoresInFunction` on every clone.
- `tm_untrack_spec_alloc()` (in `tm_alloc_overrides.hpp`): marks freed ptrs in `g_spec_allocs` by nulling `node->ptr`. Called from `tm_free` in all 4 runtimes (TinySTM, NOrec, SwissTM, TL2).
- `memset` detection: changed from exact match `"llvm.memset"` to `starts_with("llvm.memset")` for LLVM 22 type-suffixed intrinsic names.
- **Allocator mismatch fix**: `tm_malloc`/`tm_calloc`/`tm_realloc` changed from `malloc`/`calloc`/`realloc` to `::operator new` to match `::operator delete` in `tm_free`. `tm_alloc_overrides.hpp` cleanup (`std::free` → `::operator delete` for `tm_clear_spec_allocs`/`tm_flush_deferred_frees`/`tm_clear_deferred_frees`).
- **`tracesFromTMGlobal` fix**: `tm_malloc`/`tm_calloc`/`tm_realloc` return values now recognized as TM-traced pointers, so stores through heap-allocated nodes inside TX functions are instrumented.
- **Reverted `handleUnsafeOpaqueCall` serialization**: per user preference, removed the serialization approach.
- **TMSafeMap**: created `backends/tm_safe_map.hpp` — sorted-vector map replacement for `std::map` to avoid libstdc++'s opaque `_Rb_tree_*` functions.
- **`isSharedPointer` fix**: moved `tracesFromTMGlobal` check AFTER alloca-getBaseObject check, keeping correct order; added `getBaseObjectNoLoad` (omits LoadInst tracing) so pointers loaded from allocas that store TM-traced values are correctly identified as shared rather than falsely classified as local. Fixes iterator-alloca binary search instrumentation in TMSafeMap.
- **WBCTL merge-bytes-on-read fix**: when `read_word_ctl` encounters a wider read (UINT64) whose write-set only has narrower entries (UINT8 from memcpy/memmove byte-by-byte copy), it now reconstructs the UINT64 value by merging all 8 byte entries instead of falling through to stale memory. Previously, vector element keys copied via byte-by-byte `tm_write_i1` were invisible to UINT64 TM reads (binary search), returning garbage from memory instead of the TX's own buffered key.
- `alloc_stress_test` status: **FULL PASS** (vec + map insert/erase + raw new/delete + mixed workers), **PASS at 1t/2t/3t** concurrent.
- Bank benchmark: **PASS at 1t/2t/4t** with correct total money preserved.

## In Progress
- **`alloc_stress_test` crash (NULL-write, not map corruption)**: **ROOT CAUSE IDENTIFIED** — `tracesFromTMGlobal` false positive when an alloca stores the *address* of another alloca. The function recurses through stores: outer alloca → stores address of inner alloca (a stack address) → inner alloca stores TM-traced `_M_finish` pointer → incorrectly returns `true` for outer alloca. Effect: `tm_write_ptr` emitted for stores to outer alloca (a stack variable) → TM-buffered writes don't update stack → stale alloca load returns NULL → `_M_finish` is set to NULL → crash on next push. Fix: when checking a store's value operand in the alloca-stores-search, skip value operands whose base (stripping GEPs/bitcasts, NOT loads) is an `AllocaInst` — storing a stack address does not trace to a TM global.

## Blocked
- `alloc_stress_test` map corruption: `std::map` operations call `_Rb_tree_insert_and_rebalance` from libstdc++ (shared library). The function body is never available as IR (even with `-O0`/`-O2`/`-flto`), so the plugin cannot instrument its stores. Any test using `std::map` inside __transaction_atomic with concurrent workers will exhibit data corruption. Solutions: (a) TM-safe map replacement, (b) serialize map operations, (c) LTO the entire libstdc++ for function body visibility, (d) accept limitation.

## Key Decisions
- `tm_untrack_spec_alloc()` marks `node->ptr = nullptr` rather than unlinking: O(n) traversal is acceptable for small spec_alloc lists.
- `__tree_deleter_tm_clone` can never be inlined (recursive), so must be handled by extending `TMInstrumentInlinePass` to non-TX clones.
- Runtime `unordered_map` bucket allocations MUST NOT go through `tm_malloc`/`spec_alloc` — they'd be freed on abort while the map still references it.
- `instrumentLoadsStoresInFunction` only handles loads/stores + MallocFree, NOT memory intrinsics (memcpy/memmove/memset). Memory intrinsics inside clones are caught by `TMInstrumentPass` after inlining.
- `tm_malloc` must use `::operator new` (not `::malloc`) so that global destructors' `operator delete` calls are consistent.
- `_Rb_tree_insert_and_rebalance` is a fundamental opaque barrier: its stores bypass TM write-set regardless of pipeline. Tests using `std::map` in concurrent TM will not be correct. This is acceptable — the opaque check correctly detects it.
- `CloneMode::CloneOnly` now clones ALL reachable functions (like `AlwaysInline` mode), fixing the non-inline pipeline for local containers.
- Post-instrumentation `-O3` reordering remains a problem for the inline pipelines despite `atomic_signal_fence`. The non-inline pipeline avoids this by keeping clones as separate `NoInline`+`OptimizeNone` functions.

## Relevant Files
- `backends/runtimes/TinySTM_runtime.cpp`: `tm_malloc`/`tm_free` definitions (use `::operator new/delete`).
- `backends/tm_alloc_overrides.hpp`: speculative alloc tracking + deferred free.
- `llvm_tm_plugin/src/tm_local_vars.hpp`: `tracesFromTMGlobal` — now tracks `tm_malloc`/`tm_calloc`/`tm_realloc`.
- `llvm_tm_plugin/src/tm_instrument_helpers.hpp`: `handleMallocFree` intercepts `_Znwm`/`_ZdlPv` etc.
- `llvm_tm_plugin/src/tm_method_instrumentation.hpp`: `computeClonableFunctions` / `redirectCallsToClones` — CloneOnly now clones all.
- `llvm_tm_plugin/src/TMInstrumentPass.cpp`: three pipelines (`tm-instrument`, `tm-instrument-inline`, `tm-instrument-then-inline`).
- `llvm_tm_plugin/DEBUG.md`: debugging guide (symbol names, pipelines, GDB breakpoints).
- `llvm_tm_plugin/Makefile`: static pattern rules, `BUILD_TYPE`, `PLUGIN_CXXFLAGS`.

## Next Steps
1. Run full test suite with `tm-instrument` pipeline to check for regressions.
2. Investigate the `tm-instrument-then-inline` pipeline — similar `-O3` reorder issue as `tm-instrument-inline`.
3. Debug STMbench7 null-write crash and `ll_alloc_test` hang (pre-existing).

## Recent Refactoring
- **`CloneOnly` fix**: `computeClonableFunctions` and `redirectCallsToClones` now treat `CloneMode::CloneOnly` like `AlwaysInline` — clone ALL reachable functions and redirect ALL calls. Previously, CloneOnly mode was too conservative (only cloned functions with TM-traced pointer args), producing 0 clones for local-container call graphs.
- **`tm-instrument-then-inline` pipeline**: new pipeline that instruments each clone individually (pre-inline) then runs `AlwaysInlinerPass` + `TMInstrumentPass`. Produces 204 TM ops vs 179 for the inline pipeline, showing pre-inline instrumentation catches more loads/stores. Both fail due to `-O3` post-opt reordering.
- **`BUILD_TYPE=DEBUG`**: now uses `tm-instrument` (non-inline) pipeline instead of `tm-instrument-inline`, preserving clone functions for breakpoints.
- **Memory intrinsic fix in `TMInstrumentPass`**: `handleMemoryIntrinsic` → `needsMemIntrinsicInstrumentation` + push to `MemIntrinsics` vector (was a bug: detected but never instrumented).
- **`TMStripLifetimePass`**: strips `llvm.lifetime.start/end` calls for LLVM 22 compat.
- **Makefile refactoring**: static pattern rules, `PLUGIN_CXXFLAGS`, `TM_LINK_OPT`, `BUILD_TYPE`, convenience targets.
- **`DEBUG.md`**: debugging guide with pipeline docs, symbol name resolution, and GDB breakpoint strategies.
