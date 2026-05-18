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

### 5. Documentation updated
- `README.md`: opaque resolution pipeline description, new variables table, Python req
- `docs/REQUIREMENTS.md`: Python 3.8+ added, project structure updated
- `check-requirements.sh`: Python 3.8+ check added

## Key Files
- `llvm_tm_plugin/src/TMInstrumentPass.cpp` — checkOpaqueOrAbort fix
- `llvm_tm_plugin/src/opaque_safe_table.hpp` — shared known-safe table (+ `emitOpaqueSuggestion`)
- `llvm_tm_plugin/tm-resolve-opaque.py` — NEW: resolve tool
- `llvm_tm_plugin/tm_pipeline.mk` — opaque resolution integration
- `llvm_tm_plugin/test/test_math_opaque.cpp` — NEW: math opaque test
- `README.md`, `docs/REQUIREMENTS.md`, `check-requirements.sh` — docs updated

## Known Issues
- Labyrinth: TM annotation on `g_labyrinth` fixed pointer analysis (do_expansion/do_traceback now cloned and instrumented).
  Verified clean at all thread counts (1-16t, 0 aborts, verification passed).
- The earlier "crash at 2 threads" was caused by stale binary from partial file reverts during investigation.
  Clean rebuild works perfectly.

## Next Steps
1. Extend `guess_declaration_ir()` with more function signatures as needed
2. Run full benchmark suite to verify no regressions
3. Test STMbench7 (3 workloads) + TPC-C at 16 threads
