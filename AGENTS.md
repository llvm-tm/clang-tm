# TM API C++ — Project Summary

## Phase-switching backend swap

`tm_swap_runtime()` now supports runtime backend switching. Added `tm_get_real_hooks()` to retrieve registered hooks. The phase-switch test (`test_phase_switch.cpp`) demonstrates: swap from stubs to real TinySTM hooks mid-run, verify money conservation across phases.

## Dual-backend swap test (TinySTM ↔ NOrec)

`test_swap_backends.cpp` links both TinySTM and NOrec in a single binary, swapping between them at runtime. NOrec's symbols are renamed via `norec_wrapper.cpp` (`#define`-based) to avoid linker conflicts with TinySTM. The 3 retry-loop TLS variables (`tm_jmpbuf`, `tm_nested_call_counter`, `tm_longjmp_ret`) were moved to `tm_hooks.cpp` so both backends share them. `tm_swap_runtime()` now also updates `s_real_hooks` to prevent `tm_hook_init_thread()` from reverting the swap.

## Direct backend refactoring (hooks system)

All 6 direct backends (DUDETM, NVHTM, SPHT, DistributedSGL, PersistentSGL, TSXSGL) now use the hook system: removed their `extern "C"` definitions of hook functions, made them `static`, and register via `TMRealHooks` + `tm_register_real_hooks()`.

## Plugin dead code cleanup

Removed unused functions (`createHookCall`, `handleMemoryIntrinsic`, `hasTMGlobals`, `isSTLContainerFunction`, `collectDirectCalls`, `callsTransactionFunctions`, `transitivelyCallsTransactionFunctions`, `TMMethodInfo`), unused file `AtomicDoLower.cpp`, and unused runtime functions (`tm_get_type_string`, `tm_read_z`, `tm_write_z`, `consume_ptr`, commented-out `tm_setjmp`).

## Benchmark bug fixes

- **eigenbench crash**: `thread_exit()` was inside the `while (!g_stop.load())` loop; after the first iteration `current_tx` was cleared and the next `transaction()` call crashed in `norec::begin()`. Fixed by moving `thread_exit()` outside the loop.
- **rbtree double‑free**: `tm_insert()` frees `z` on duplicate key at line 132, but caller `worker()` at line 244 freed `z` again. Removed the duplicate free from the caller.

## LLVM race checker plugin

`plugin/passes/TMRaceCheckerPass.cpp` — a standalone `opt` pass (`-passes="tm-race-checker"`) that scans all non‑transaction functions for loads/stores to TM‑annotated globals. Reuses `tracesFromTMGlobal()` from `tm_local_vars.hpp` (same analysis as the instrumentation pipeline). Emits source‑location warnings suggesting `[[tx::transaction]]` annotation.

The instrumentation plugin (`libTMInstrument.so`) now also warns about missing transaction annotations at build time via `checkMissingTransactionAnnotations()` in `tm_instrument_helpers.hpp`, called from `setupModulePass()`.

Usage:
```sh
opt-22 -load-pass-plugin=plugin/bin/libTMRaceChecker.so \
       -passes="tm-race-checker" myapp.bc -o /dev/null
```

## --version / ASCII art

`plugin/bin/tm-race-checker` (shell wrapper) and `fuzz-counter`/`fuzz-bank` (C++ benchmarks) support `--version` / `-V`. Each prints box‑drawing letter art:

- **tm-race-checker** — "RACE" (leading ━) + "CHECKER" (7‑block)  
- **fuzz-counter**, **fuzz-bank** — "FUZZ" (4‑block)

## TM memory debug allocation

`tm_region_check_leaks()` added to `tm_region_allocator.hpp`, gated by `-DTM_DEBUG_ALLOC`. When enabled, a per‑thread `unordered_map<void*,size_t>` tracks live allocations; `tm_region_check_leaks()` at exit prints any unfreed pointers.

## Verified passing (NOREC backend)

| Benchmark   | Status |
|-------------|--------|
| `test_tx`   | 114/114 PASS |
| `test_ds`   | 207/207 PASS |
| `eigenbench`| PASS |
| `rbtree`    | PASS |
| `ycsb`      | PASS |
| `intruder`  | PASS |

## Known issues

- `test_stress_ds` has a pre-existing assertion failure on non-TM addresses (region-size bug unrelated to hooks refactoring)
- TinySTM `counter_mt` has the same pre-existing assertion failure
- **rbtree double‑free in TM region allocator** (`FATAL: double-free detected in TM`) — pre‑existing. Root cause: region allocator reuses addresses across transactions; `g_deferred_frees_set` (thread‑local) fires false‑positive when same address freed again in a different thread.
- **stmbench7 times out with >1 thread** — root cause: data race in `ts_multimap::lower_bound()` — `op_st5` drops `std::mutex` before iterating, leaving raw iterator into `tm_malloc`‑backed memory. Affects NOrec, TL2, SwissTM; TinySTM survives by chance (per‑object locking serializes the path).
