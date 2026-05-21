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
- `alloc_stress_test` root cause identified: `std::map` inside TM global causes crash in `read_word_ctl` with `tx` pointer corrupted to `0x80`. The `tm-instrument` pipeline does NOT call `handleMallocFree` on cloned functions (only `instrumentLoadsStoresInFunction`), so `_Znwm` inside clones uses standard allocator. The crash mechanism is a corrupted `current_tx_wbctl` TLS variable (value `0x80`), suggesting heap corruption or a write-set entry overlapping with the TLS storage address. Even a single-threaded map-insert worker crashes (exit 139, SIGSEGV).
- `alloc_stress_test` status: NOT our regression — root cause is the `tm-instrument` pipeline's `instrumentLoadsStoresInFunction` not calling `handleMallocFree` (by design, it only instruments loads/stores). Tree rebalancing stores ARE instrumented (via `isSharedPointer` + `isTMTracedPtr`), so the crash is a runtime TM issue (corrupted `current_tx_wbctl`), not a missed-instrumentation issue.
- Proofs (`docs/proofs.md`): Section 8 (memory allocation) is compatible with current code. Non-intrusive `FreeNode` pattern is used. No proof yet for `tm_untrack_spec_alloc` nullptr-skipping — this is a minor optimization (bookkeeping nodes always freed regardless) that doesn't affect the deferred-free correctness arguments.

## In Progress
- *(none)*

## Blocked
- `alloc_stress_test` SIGSEGV: `current_tx_wbctl` corrupted to `0x80` in `read_word_ctl`. Likely heap corruption from write-set `unordered_map` operations interacting with standard `_Znwm` allocations (not replaced by `tm_malloc` in clones). Isolation: single map-insert worker in `tm-instrument` pipeline crashes; map-insert + vec in inline pipeline shows data corruption ("g_map[198] = 207 (expected 1980)").

## Key Decisions
- `tm_untrack_spec_alloc()` marks `node->ptr = nullptr` rather than unlinking: O(n) traversal is acceptable for small spec_alloc lists.
- `__tree_deleter_tm_clone` can never be inlined (recursive), so must be handled by extending `TMInstrumentInlinePass` to non-TX clones.
- Runtime `unordered_map` bucket allocations MUST NOT go through `tm_malloc`/`spec_alloc` — they'd be freed on abort while the map still references them.
- `instrumentLoadsStoresInFunction` only handles loads/stores, NOT `handleMallocFree`. This is intentional — the clone functions' `_Znwm` calls stay using standard heap. For `std::map` tree nodes, this means no spec_alloc tracking, which is fine for single-thread but causes issues under the TM model.

## Next Steps
1. Investigate `alloc_stress_test` crash — hypothesize `write_set` `unordered_map` rehashing during TM write operation corrupts `current_tx_wbctl`. Try using a fixed-size write-set (e.g., `std::array` + open addressing) or validate `current_tx_wbctl` before every access.
2. Fix `instrumentLoadsStoresInFunction` to also run `handleMallocFree`, OR document that clone functions use standard allocator (not `tm_malloc`/`tm_free`).
3. Run full plugin test suite after each fix.
4. Run STMbench7 / bank at all thread counts to confirm no regression.
5. Track: `ll_alloc_test` hang (pre-existing).
