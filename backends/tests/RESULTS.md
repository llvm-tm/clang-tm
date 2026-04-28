# Backend Runtimes Analysis

## Runtime Status (Updated)

| Runtime | Has tm_get_env | Has tm_set_env | Symbol ID Params | Notes |
|---------|---------------|----------------|------------------|-------|
| TinySTM | ✅ | ✅ | ✅ | Working correctly |
| TL2 | ❌ Added | ❌ Added | ❌ Fixed | Fixed now |
| SwissTM | ❌ Added | ❌ Added | ✅ | Fixed now |
| SingleGlobalLock | ❌ Added | ❌ Added | ✅ | Fixed now |

## TinySTM Flavors Jump-Back Support

| Flavor | File | Uses siglongjmp? | Abort Mechanism |
|--------|------|------------------|------------------|
| Base | `tinystm.hpp` | ✅ Yes (line 220) | Undo log + longjmp |
| WB-ETL | `tinystm_wbetl.hpp` | ❌ No | Sets aborted flag + sleep |
| WT | `tinystm_wt.hpp` | ❌ No | Undo write + abort counter |
| WB-CTL | `tinystm_wbctl.hpp` | ❌ No | Just clears state |

**Only the base `tinystm.hpp` implements proper jump-back via siglongjmp.**

All other flavors rely on flag-based retry (while loop).

## Tests in backends/tests

| Test | Retry Mechanism | Status |
|------|-----------------|--------|
| test_runtime_simple.cpp | `while (!committed)` | Works (lost updates bug) |
| test_runtime_simple2.cpp | `setjmp` + `while` | Works |
| test_jump_back.cpp | `sigsetjmp` + `siglongjmp` | Needs testing |
| test_tinystm_simple.cpp | `setjmp` + `stm_set_env` | Works |
| Other tests | Various setjmp patterns | Not tested |

## Files Changed

1. `TL2_runtime.cpp` - Added tm_get_env, tm_set_env, fixed symbol_id params
2. `SwissTM_runtime.cpp` - Added tm_get_env, tm_set_env
3. `SingleGlobalLock_runtime.cpp` - Added tm_get_env, tm_set_env

## Known Issues

1. **TinySTM lost updates bug**: Read-set validation doesn't properly detect concurrent modifications in multi-threaded tests
2. **TinySTM WT and WB-CTL**: Don't compile (missing Lock type definitions)
3. **WB-ETL, WT, WB-CTL**: Don't use siglongjmp - rely on flag-based retry