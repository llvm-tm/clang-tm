# TM API C++ — Project Summary

## Phase-switching backend swap

`tm_swap_runtime()` now supports runtime backend switching. Added `tm_get_real_hooks()` to retrieve registered hooks. The phase-switch test (`test_phase_switch.cpp`) demonstrates: swap from stubs to real TinySTM hooks mid-run, verify money conservation across phases.

## Dual-backend swap test (TinySTM ↔ NOrec)

`test_swap_backends.cpp` links both TinySTM and NOrec in a single binary, swapping between them at runtime. NOrec's symbols are renamed via `norec_wrapper.cpp` (`#define`-based) to avoid linker conflicts with TinySTM. The 3 retry-loop TLS variables (`tm_jmpbuf`, `tm_nested_call_counter`, `tm_longjmp_ret`) were moved to `tm_hooks.cpp` so both backends share them. `tm_swap_runtime()` now also updates `s_real_hooks` to prevent `tm_hook_init_thread()` from reverting the swap.

## Direct backend refactoring (hooks system)

All 6 direct backends (DUDETM, NVHTM, SPHT, DistributedSGL, PersistentSGL, TSXSGL) now use the hook system: removed their `extern "C"` definitions of hook functions, made them `static`, and register via `TMRealHooks` + `tm_register_real_hooks()`.

## Plugin dead code cleanup

Removed unused functions (`createHookCall`, `handleMemoryIntrinsic`, `hasTMGlobals`, `isSTLContainerFunction`, `collectDirectCalls`, `callsTransactionFunctions`, `transitivelyCallsTransactionFunctions`, `TMMethodInfo`), unused file `AtomicDoLower.cpp`, and unused runtime functions (`tm_get_type_string`, `tm_read_z`, `tm_write_z`, `consume_ptr`, commented-out `tm_setjmp`).

## Known issues

- `test_stress_ds` has a pre-existing assertion failure on non-TM addresses (region-size bug unrelated to hooks refactoring)
- TinySTM `counter_mt` has the same pre-existing assertion failure
