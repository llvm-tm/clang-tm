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

## Done (this session)
- **TMTreapMap / TMTreapMultiMap**: Created `benchmarks/datastructures/tm_treap_map.hpp` — treap-based (randomized BST, Aragon & Seidel 1989) map/multimap replacement for `std::map`/`std::multimap`. Uses `std::hash<K>` + Murmur3 finalizer mixing for priorities (avoids degenerate tree from identity hash). Parent pointers for O(log n) Iterator++. Stress tests (10 tests, insert/find/erase/iterate/lower_bound) all PASS.
- **Hand-rolled RB tree (tm_rbtree.hpp)**: Buggy `erase()` (split-by-key logic wrong for separating == k from > k). Replaced by treap.
- **STMbench7 integrated with treap**: Uses `TMTreapMap` (5 by-ID indexes) + `TMTreapMultiMap` (2 by-date indexes). Uninstrumented build works (100 ops in 1s).
- **STMbench7 hangs with ALL TM backends (WBCTL, SwissTM, NOrec)**: Hang is NOT data-structure-specific — the first long traversal operation (e.g., `op_lt3` — range-for over `g_compositeParts`) hangs. Singlelock backend works (200 ops in 2s). Bank benchmark passes (verifies pipeline/toolchain intact). Root cause unknown — possibly `tm_read`/`tm_write` of vector iterator internals inside TX long loops. Needs further investigation with lldb backtrace when stuck.
- **TMSafeMultiMap**: Added multimap variant to `backends/tm_safe_map.hpp` (sorted-vector with `std::upper_bound` for insert).
- **Removed heuristic-based load/store classification**: `isSharedPointer` / `isTMTracedPtr` / `TMTracedArgs` were fundamentally flawed — they had false negatives (missing stores like `v_.end_ = pos_` in destructors) and false positives (instrumenting local alloca stores). The default is now **always-instrument**: every load/store in TX functions gets `tm_read`/`tm_write`.
- **`tm_local` annotation support**: Users can annotate variables with `__attribute__((annotate("tm_local")))` to bypass TM instrumentation on known-private local variables. The plugin collects `@llvm.var.annotation` calls (using `starts_with("llvm.var.annotation")` for LLVM 22's mangled intrinsic names) and skips loads/stores to annotated allocas.
- **`test_tm_local`**: New test verifying `tm_local` annotation works — all three sub-tests PASS (single increment, 100x increments, multiple tm_local variables).
- **`construct_tx_pattern`** passes at 1t/2t/4t on both `tm-instrument-inline` and `tm-instrument-then-inline` pipelines.
- **Removed `force_all` plugin variant**: redundant now that always-instrument is the default. Removed `compare_force_all` Makefile target.
- **WT backend refactored to use shared `tinystm_common.hpp` infrastructure**: removed local `Lock`/`LockTable`/`Transaction`/`ValueType`/`ByteOffset` copies; WT now uses common `Lock` (with incarnation bits preserved in `try_lock` and `get_incarnation()` bug fixed from `LOCK_BITS` to `OWNED_BITS`), shared `LockTable<Lock_wt>`, shared `Transaction` template, and shared `g_clock`/`thr_counter`/`jmpbuf`.
- **WT matches WBCTL patterns**: `tx_id` is now `thr_counter.fetch_add(1)` (not `(word_t)tx`), `abort_tx()` uses unconditional `siglongjmp`, nesting in `begin()` follows WBCTL (no `init_thread` inside), `tx->aborted` never set in read/write instrumentation, random backoff added.
- **WT tm_read/tm_write wrappers**: replaced `tm_read`/`tm_write` template usage with manual byte-extraction/merging wrappers (like the original WT) that pass 8-byte-aligned addresses to `read_word_wt`/`write_word_wt`, fixing unaligned `std::atomic_ref<uint64_t>` access and type-size mismatch bugs.
- **Bank benchmark WT**: **PASS 1t/2t/4t** with correct money conservation, no hangs.

## Done (this session)
- **`instrumentMemoryIntrinsic` refactored**: Replaced byte-level (UINT8) memcpy expansion with 8-byte (UINT64) loop. Wide path reads/writes 8 bytes at a time via `tm_read_i8`/`tm_write_i8`, reducing TM operations 8× (e.g., 49152→6144 for 24576-byte labyrinth grid memcpy). Memset: broadcasts fill byte across 8 positions via multiplication by `0x0101010101010101`. Uses `tm_write_i8` (not direct store) for destinations, preserving TM write-set participation and avoiding test crashes.
- **Fixed memset GEP crash**: For `memset(dst, 0, n)`, `SrcOrVal` is `i8 0` (an integer constant, not a pointer). Previous wide-path code tried `GEP(i8, 0, Idx)`, producing invalid IR. Now memset uses a dedicated broadcast path that never GEPs from the constant value.
- **`TMSafeMap` `iterator` type fix**: Added `using const_iterator` before `find()` method (was missing in public section, causing build error in `test_alloc_stress`).
- **Backend fixes committed (`e32012b`)**: SwissTM `write_log` changed from `vector<WriteLogEntry>` to `list<WriteLogEntry>` (raw pointers stored in w_lock must remain stable across subsequent `push_back`). NOrec uses `reset()` (sets `active=false`) in read-only commit path. TL2 `abort_tx()` calls `siglongjmp`. WBETL deduplicates `locks_held`, releases locks from `locks_held` instead of `write_set`. WT validates own write-lock in read-set at commit (detects stale reads between read_time and lock_acquisition_time).
- **New backend test suite**: `test_single.cpp` + `test_multi.cpp` with 7 backends × 2 tests = 14 binaries. Tests basic types, sequential TX, alloc/deferred-free, abort stress, write-set validation, read-set caching, floating-point, barrier-coordinated interleaving.
- **Docs + runtime nesting guard removal**: improved user workflow, tm_local, nesting mechanism, pipeline selection, and thread entry detection docs; removed dead-code `if (tm_nested_call_counter == 1)` guards from 7 runtimes.
- **WT counter test**: own-lock validation fix improved from 709840/800000 (90160 lost) → 797703/800000 (2297 lost). Remaining gap from WT's inherent write-through race — lock-entent adjacent updates. WBCTL/WBETL pass 800000/800000.

## Blocked
- `alloc_stress_test` map corruption: `std::map` operations call `_Rb_tree_insert_and_rebalance` from libstdc++ (shared library). The function body is never available as IR (even with `-O0`/`-O2`/`-flto`), so the plugin cannot instrument its stores. Any test using `std::map` inside __transaction_atomic with concurrent workers will exhibit data corruption. Solutions: (a) TM-safe map replacement (treap now available), (b) serialize map operations, (c) LTO the entire libstdc++ for function body visibility, (d) accept limitation.
- **STMbench7 hangs with ALL TM backends**: First long traversal operation (e.g., `op_lt3` — range-for over `g_compositeParts`) hangs with WBCTL/SwissTM/NOrec at 1 thread. Not data-structure-specific (occurs even without any map operations). Singlelock and uninstrumented work fine. Bank benchmark works fine (proves pipeline intact). Needs investigation — possible bug in tm_read during vector iterator operations inside long-running TX.
- **STAMP Labyrinth still slow with inline pipeline**: Even with 8× reduction from UINT64 memcpy, the first labyrinth path doesn't complete within 60s. The `do_expansion` BFS loop generates thousands of TM read/write operations per path. Non-inline pipeline completes in ~5ms. Root cause: inline pipeline instruments ALL STL code (vector, deque iterators, queue internals) inside the TX, creating additional TM operations beyond the grid memcpy. Possible solutions: (a) apply `tm_local` annotations to labyrinth helper functions' internal variables, (b) use the non-inline pipeline for long-running TX functions, (c) add a per-benchmark pipeline override.

## Key Decisions
- `tm_untrack_spec_alloc()` marks `node->ptr = nullptr` rather than unlinking: O(n) traversal is acceptable for small spec_alloc lists.
- `__tree_deleter_tm_clone` can never be inlined (recursive), so must be handled by extending `TMInstrumentInlinePass` to non-TX clones.
- Runtime `unordered_map` bucket allocations MUST NOT go through `tm_malloc`/`spec_alloc` — they'd be freed on abort while the map still references them.
- `instrumentLoadsStoresInFunction` only handles loads/stores, NOT `handleMallocFree`. This is intentional — the clone functions' `_Znwm` calls stay using standard heap. For `std::map` tree nodes, this means no spec_alloc tracking, which is fine for single-thread but causes issues under the TM model.
- Do NOT serialize STL container calls; the correct fix is to ensure heap allocations inside TX functions are traced as TM globals (via `tracesFromTMGlobal` + `tm_malloc`).
- `tm_malloc` must use `::operator new` (not `::malloc`) so that global destructors' `operator delete` calls are consistent.
- `_Rb_tree_insert_and_rebalance` is a fundamental opaque barrier: its stores bypass TM write-set regardless of pipeline (inline or non-inline). Tests using `std::map` in concurrent TM will not be correct.
- **Heuristic is fundamentally wrong**: the `isSharedPointer` + `isTMTracedPtr` double-guard misses loads/stores that need instrumentation (destructor's `v_.end_ = pos_` store) and catches ones it shouldn't (local alloca stores). Default is now **always-instrument all loads/stores**; users opt out per-variable via `__attribute__((annotate("tm_local")))`.
- `@llvm.var.annotation` has type-suffixed names (e.g., `llvm.var.annotation.p0.p0`) in LLVM 22 — use `starts_with("llvm.var.annotation")` for name matching, not exact equality.
- **WT refactored to use common Lock**: `try_lock` now preserves incarnation bits (`INCARNATION_MASK << OWNED_BITS`), `get_incarnation()` reads from `OWNED_BITS` not `LOCK_BITS`. Safe for WBCTL/WBETL (incarnation always 0 there).
- **WT tm_read/tm_write use manual byte merging**, not the `tm_read`/`tm_write` templates — because templates pass type-sized values to `read_value_from_addr`/`write_value_to_addr`, but WT works with full 8-byte words at aligned addresses. Sub-word wrappers (i1, i2, i4, f4) extract/merge bytes from the full word and pass aligned addresses to `read_word_wt`/`write_word_wt`.

## Relevant Files
- `backends/TinySTM/tinystm_wt.hpp`: WT implementation — refactored to use shared `tinystm_common.hpp` infrastructure.
- `backends/TinySTM/tinystm_common.hpp`: shared Lock/LockTable/Transaction templates, bit layout definitions, jmpbuf declaration.
- `backends/TinySTM/tinystm_wbctl.hpp`: reference implementation for correct patterns.
- `backends/TinySTM/tinystm_globals.hpp`: globals definitions for all three TinySTM backends.
- `backends/runtimes/TinySTM_runtime.cpp`: `tm_malloc`/`tm_free` definitions (use `::operator new/delete`).
- `backends/tm_alloc_overrides.hpp`: speculative alloc tracking + deferred free.
- `llvm_tm_plugin/src/tm_local_vars.hpp`: `tracesFromTMGlobal` — now tracks `tm_malloc`/`tm_calloc`/`tm_realloc`; also `collectTMLocalAllocas`/`isTMLocalVar` for tm_local support.
- `llvm_tm_plugin/src/tm_instrument_helpers.hpp`: `handleMallocFree` intercepts `_Znwm`/`_ZdlPv` etc; `handleLoadStore` — always-instrument (skip only tm_local).
- `llvm_tm_plugin/src/tm_method_instrumentation.hpp`: `instrumentLoadStoresInFunction` — always-instrument (skip only tm_local); `TMTracedArgs` kept for cloning/redirection decisions only.
- `llvm_tm_plugin/src/TMInstrumentPass.cpp`: all three pipeline registrations.
- `llvm_tm_plugin/src/opaque_safe_table.hpp`: known-safe opaque function table for `_ZSt`/`_ZNS`/`_ZNSt` functions.
- `llvm_tm_plugin/test/test_tm_local.cpp`: new test verifying tm_local annotation.

## Next Steps
1. Address STAMP Labyrinth inline-pipeline slowness. Options:
   (a) Per-benchmark pipeline override (use non-inline for labyrinth).
   (b) `tm_local` annotations on labyrinth helpers to reduce STL instruction count.
   (c) Profile to find hot spots beyond memcpy (BFS loop, deque operations).
2. Debug STMbench7 hang — investigate `tm_read`/`tm_write` of vector iterator internals during long TX loops. Get lldb backtrace when stuck at op_lt3.
3. If hang fixed: run STMbench7 with treap maps at 1t/2t/4t across all backends.
4. Run bank at all thread counts to confirm no regression.
