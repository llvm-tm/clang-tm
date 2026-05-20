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
  Multi-threaded (4+ threads) crashes: pre-existing, in `_M_insert_unique_node` — likely unordered_map concurrent access issue or multithreaded TM coordination bug.
  `SingleGlobalLock` backend works fine at all thread counts.

## Next Steps
1. Debug TinySTM backend multithreaded crash in STMbench7 (pre-existing, separate issue from the fixed 1t bug)
2. Extend `guess_declaration_ir()` with more function signatures as needed
3. Run full benchmark suite to verify no regressions
4. Test STMbench7 (3 workloads) + TPC-C at 16 threads
