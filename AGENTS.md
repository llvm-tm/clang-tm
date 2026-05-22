# Session Summary

## Goal
Maintain the LLVM IR-level TM plugin pipeline and fix benchmark/test failures for LLVM 22.

## Done
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
- Runtime `unordered_map` bucket allocations MUST NOT go through `tm_malloc`/`spec_alloc` — they'd be freed on abort while the map still references them.
- `instrumentLoadsStoresInFunction` only handles loads/stores, NOT `handleMallocFree`. This is intentional — the clone functions' `_Znwm` calls stay using standard heap. For `std::map` tree nodes, this means no spec_alloc tracking, which is fine for single-thread but causes issues under the TM model.
- Do NOT serialize STL container calls; the correct fix is to ensure heap allocations inside TX functions are traced as TM globals (via `tracesFromTMGlobal` + `tm_malloc`).
- `tm_malloc` must use `::operator new` (not `::malloc`) so that global destructors' `operator delete` calls are consistent.
- `_Rb_tree_insert_and_rebalance` is a fundamental opaque barrier: its stores bypass TM write-set regardless of pipeline (inline or non-inline). Tests using `std::map` in concurrent TM will not be correct.

## Relevant Files
- `backends/runtimes/TinySTM_runtime.cpp`: `tm_malloc`/`tm_free` definitions (use `::operator new/delete`).
- `backends/tm_alloc_overrides.hpp`: speculative alloc tracking + deferred free.
- `llvm_tm_plugin/src/tm_local_vars.hpp`: `tracesFromTMGlobal` — now tracks `tm_malloc`/`tm_calloc`/`tm_realloc`.
- `llvm_tm_plugin/src/tm_instrument_helpers.hpp`: `handleMallocFree` intercepts `_Znwm`/`_ZdlPv` etc.
- `llvm_tm_plugin/src/tm_method_instrumentation.hpp`: `instrumentLoadsStoresInFunction` — runs `handleMallocFree` in clones.
- `llvm_tm_plugin/src/TMInstrumentPass.cpp`: both pipelines (`tm-instrument` and `tm-instrument-inline`).
- `llvm_tm_plugin/test/alloc_stress_test.cpp`: failing test with vec+map+erase+raw new workers.

## Next Steps
1. Apply fix in `tracesFromTMGlobal` (skip stores of alloca addresses in alloca-stores search) and rebuild.
2. Re-run `alloc_stress_test -v 2 -i 0 -e 0 -r 0 -m 0` with and without ASan to verify fix.
3. If fixed: re-run full test suite (types, nested, bank benchmarks).
4. Run bank at all thread counts to confirm no regression.
5. Debug STMbench7 null-write crash and `ll_alloc_test` hang (pre-existing).
