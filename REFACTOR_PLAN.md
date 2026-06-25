# Refactoring Plan

## Scope
Eliminate duplicated code across C++ backends (~2,000 lines), Rust runtime (~500 lines), and plugin (~300 lines).

## Phase 1: C++ Backend Common Pattern Extraction

### 1a. Create `backends/tm_impl/common/tm_backend_macros.hpp`
Shared macros for the 3 largest duplication sources:

**`TM_DEFINE_READ_WRITE_HOOKS(ns)`** — Generates 14 static wrapper functions (`real_tm_read_i1` through `real_tm_write_ptr`) that delegate to `ns::tm_read_i1(...)`, etc. Saves ~14 lines per backend × 14 backends = ~200 lines.

**`TM_DEFINE_PLUGIN_RW(ns)`** — Generates the 6 extern "C" functions (`tm_read_i16`, `tm_read_i32`, `tm_read_i64`, `tm_write_i16`, `tm_write_i32`, `tm_write_i64`) plus `tm_read_z`, `tm_write_z`, `tm_memset` for plugin-instrumented binaries. The read_i32/i64 and write_i32/i64 use the standard loop pattern (word-at-a-time through `ns::tm_read_i8`/`ns::tm_write_i8`). Saves ~65 lines per backend × 9 backends = ~585 lines.

**`TM_REAL_HOOKS_TABLE(prefix)`** — Generates the `const TMRealHooks g_<prefix>_hooks = { .begin = real_tm_begin, ... }` designated-initializer table. Saves ~22 lines × 14 backends = ~308 lines.

### 1b. Move shared TLS + mutex + stubs to `tm_alloc_overrides.hpp` / `tm_hooks.cpp`
The thread-local `g_serialize_mutex` + `tm_serialize_lock/unlock` and the `tm_setjmp`/`tm_set_env` stubs are identical across 10+ backends. Provide weak definitions in `tm_hooks.cpp` so backends can drop their copies. Saves ~12 lines × 10 backends = ~120 lines.

### 1c. Update all 14 backend runtime files
Each backend replaces:
- 14 read/write functions → `TM_DEFINE_READ_WRITE_HOOKS(ns)`
- 6 extern "C" plugin functions → `TM_DEFINE_PLUGIN_RW(ns)`
- 22-line TMRealHooks table → `TM_REAL_HOOKS_TABLE(prefix)`
- TLS extern declarations → remove (already in shared headers)
- `g_serialize_mutex`/lock/unlock → remove (weak default in tm_hooks.cpp)
- `tm_setjmp`/`tm_set_env` → remove (weak default in tm_hooks.cpp)

Also remove the 4 thread-local variables from each backend:
- `thread_local bool g_in_tx = false;`
- `thread_local FreeNode* g_deferred_frees = nullptr;`
- `thread_local std::unordered_set<void*> g_deferred_frees_set;`
- `thread_local SpecAlloc* g_spec_allocs = nullptr;`
These are already declared as `extern thread_local` in `tm_alloc_overrides.hpp`. Move their definitions to `tm_hooks.cpp` and delete from all 14 backends.

## Phase 2: Rust runtime_core Shared Utilities

### 2a. Move `apply_typed_value` to `runtime_core`
Remove from `runtime/tinystm/src/wbctl.rs`, `runtime/norec/src/lib.rs`, `runtime/tl2/src/lib.rs`. Add as `pub unsafe fn apply_typed_value(addr: usize, tv: &TypedValue)` in `runtime_core`. Saves ~39 lines.

### 2b. Remove `byte_size_of_tv` — use `TypedValue::byte_size()` directly
Exists in `runtime/norec/src/lib.rs` and `runtime/swisstm/src/lib.rs`. Replace calls with `tv.byte_size() as u8`. `TypedValue` already has `byte_size(&self) -> usize`. Saves ~18 lines.

### 2c. Move `read_mem_val` to `runtime_core`
Exists in `runtime/norec/src/lib.rs` and `runtime/swisstm/src/lib.rs`. Add as `pub unsafe fn read_mem_val(addr: usize, sz: u8) -> u64` in `runtime_core`. Saves ~22 lines.

## Phase 3: Plugin Common Code Extraction

### 3a. Extract instruction-iteration loop helper
The 3-pass loop (memintrinsic → malloc/free → load/store) is identical in `TMInstrumentPass.cpp`, `TMInstrumentFnPass.cpp`, and `TMInstrumentInlinePass.cpp`. Extract to `tm_instrument_helpers.hpp` as `instrumentFunctionBody(Function&, Module&, TMRuntimeHooks&, ...)`. Saves ~60 lines.

### 3b. Extract `findClone` usage
`findClone()` exists in `TMInstrumentPass.cpp` line 347 but is only used locally. Three other passes have inline clone-name-skip checks. Move `findClone` to `tm_instrument_helpers.hpp` and use it everywhere. Saves ~15 lines.

### 3c. Move `isTLSGlobal`/`isThreadStateAccess` to shared header
These lambdas are duplicated in `tm_instrument_helpers.hpp` and `TMCleanupPass.cpp`. Move to `tm_instrument_helpers.hpp` as inline functions. Saves ~20 lines.

### 3d. Merge `findTMGlobalName` into `tracesFromTMGlobal`
Make `tracesFromTMGlobal` return the `GlobalVariable*` instead of `bool`, eliminating the need for `findTMGlobalName` in `TMRaceCheckerPass.cpp`. Saves ~80 lines.

## Verification
After each phase:
- `make -C benchmarks/cpp -j4 run-test-tx` — verify all 114 C++ tests pass
- `smoke_test.sh` — verify all 36/36 backends build
- `cargo build --release` (Rust workspace) — verify zero warnings
- `make -C plugin run` — verify plugin tests pass

## Order of Implementation
Phase 1a (macros) → Phase 1b (shared TLS/mutex/stubs) → Phase 1c (update 14 backends)
→ Phase 2 (Rust) → Phase 3 (Plugin) → Final verification
