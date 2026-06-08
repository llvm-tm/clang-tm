# Debug Build & GDB

## Quick start

```sh
# Build test binary with debug info (plugin assertions enabled, -O0, debug symbols)
BUILD_TYPE=DEBUG make test_local_containers

# Run under GDB
BUILD_TYPE=DEBUG gdb --args ./bin/test_local_containers 4 100 100
```

## What BUILD_TYPE=DEBUG does

| Aspect | RELEASE (default) | DEBUG |
|---|---|---|
| Plugin variant | `libTMInstrument_release.so` (`-DNDEBUG`) | `libTMInstrument_debug.so` (assertions enabled, verbose TM_DEBUG output) |
| Pipeline | `tm-instrument-inline` (inlines clones, then instruments) | `tm-instrument` (no inlining — clones survive, breakpoint-able) |
| Post-instrumentation opt | `opt -O3` | `opt -O0` (preserves IR structure) |
| Link flags | `-O1` | `-O0 -g` (debug symbols, no optimizations) |

## Example: debug a specific test

```sh
# Build
BUILD_TYPE=DEBUG make test_vector_realloc

# Run with verbose plugin logging
BUILD_TYPE=DEBUG gdb -ex run --args ./bin/test_vector_realloc 2 100

# Run with custom thread count / iterations
BUILD_TYPE=DEBUG ./bin/test_local_containers 4 1000 10
```

## Plugin debug logging

The debug plugin variant (`libTMInstrument_debug.so`) enables `TM_DEBUG()` output lines like:

```
[TM Plugin] isSharedPointer: Pointer traces to TM global → shared
[TM Plugin] instrumentLoadsStoresInFunction: processing _Z9vector_txii_tm_clone
```

These appear during the `opt -load-pass-plugin=...` step. To capture them separately:

```sh
BUILD_TYPE=DEBUG make out/test_foo.instr.bc 2> /tmp/plugin_debug.log
```

## Rebuilding the plugin

The debug plugin is rebuilt automatically when `BUILD_TYPE=DEBUG` is set. To force a rebuild:

```sh
rm bin/libTMInstrument_debug.so && BUILD_TYPE=DEBUG make test_foo
```

## Breakpoints & Symbol Names

### Cloned functions are inlined — they don't exist in the final binary

The **inline pipeline** (`tm-instrument-inline`, the **default**):
1. Clones all reachable callees with `alwaysinline` → name like `_Z6map_txii_tm_clone`
2. Runs `AlwaysInlinerPass` → inlines them into the TX function
3. Runs `-O3` post-opt → may inline even more

**Result:** no `*_tm_clone` functions survive in the final binary. Breakpoints on them fail.

### Non-inline pipeline (preserves clones for debugging)

The **non-inline pipeline** (`tm-instrument` or `tm-instrument-debug`):
1. Clones callees and instruments loads/stores inline — **no `AlwaysInlinerPass`**
2. Clones survive as separate functions in the binary with `_tm_clone` suffix
3. Works at `-O0` (doesn't need the inliner)

```sh
# Build with non-inline pipeline (preserves clone functions in binary)
TM_INSTRUMENT_PIPELINE=tm-instrument make test_local_containers
BUILD_TYPE=DEBUG TM_INSTRUMENT_PIPELINE=tm-instrument make test_local_containers
```

With this pipeline, `*_tm_clone` functions exist and are breakpoint-able:

```gdb
b _Z6map_txii_tm_clone
b _Z9vector_txii_tm_clone
```

**Downside:** callee internals (e.g., `std::vector::push_back`) are NOT inlined, so their loads/stores are only instrumented if the callee itself is cloned. The non-inline pipeline only instruments what's directly in the TX function body at the IR level.

### How to find the actual symbol names

Check what's in the binary:

```sh
# List all TX function entry points
nm -a bin/test_local_containers | grep -E '^[0-9a-f]+ T '

# Find specific function (mangled C++ name)
nm bin/test_local_containers | grep map_tx
# → 0000000000063de0 T _Z6map_txii
```

Common mangled names in tests:

| Source name | Mangled symbol |
|---|---|
| `void map_tx(int, int)` | `_Z6map_txii` |
| `void vector_tx(int, int)` | `_Z9vector_txii` |
| `void push_tx(int, int)` | `_Z7push_txii` |

### Recommended breakpoints

```gdb
# TX function entry
b _Z6map_txii

# Line number in test source
b test_local_containers.cpp:55

# TM runtime operations (from backends/TinySTM/tinystm_wbctl.hpp)
b tm_read_i8
b tm_write_i8
b tm_read_ptr
b tm_write_ptr
b tm_malloc
b tm_free
b tm_begin
b tm_end
b tinystm::commit

# Thread-specific (GDB 7.0+)
b _Z6map_txii if thread_id == 3 && base == 15000
b tm_write_i8 if thread_id == 3
```

### Viewing intermediate IR (pre-inline) to see clones

```sh
# Stop after instrumentation, before -O3 inlining
make out/test_local_containers.instr.bc

# Disassemble to IR
llvm-dis out/test_local_containers.instr.bc -o /tmp/instr.ll
```

Then search for clone functions:

```sh
grep 'define.*_tm_clone' /tmp/instr.ll
```

This shows the cloned functions with their `alwaysinline` attribute before they're inlined.

### Viewing the final instrumented TX function

```sh
llvm-dis out/test_local_containers.opt.bc -o /tmp/opt.ll
grep -A 200 'define.*_Z6map_txii' /tmp/opt.ll
```

Look for `call void @tm_write_ptr`, `call void @tm_read_i8` etc. to confirm instrumentation is present.

## Common issues

### `Intrinsic has incorrect argument type!` on `llvm.lifetime.start/end`

The pipeline strips these automatically via `TMStripLifetimePass`. If you see this error, your system `clang++` (LLVM 21) is incompatible with LLVM 22 bitcode. Use the Makefile targets (which use LLVM 22 tools) instead of invoking `clang++` directly.

### Plugin not found

```sh
make bin/libTMInstrument.so
```

### Missing runtime symbols

Check the `DEFAULT_LINK_FLAGS` in `Makefile` include `-pthread` and the correct backend runtime file (e.g., `TinySTM_runtime.cpp` for TinySTM-backed tests).

## Valgrind Helgrind/DRD for Non-TM Concurrency Debugging

The TM plugin instruments **TM-annotated** accesses (`TX` functions on `struct TM` globals). Code protected by manual synchronization (e.g., `tm_serialize_lock/unlock`, `std::mutex`, atomics) is NOT instrumented — the TM runtime does not track these accesses, so they can race silently.

Use Valgrind's **Helgrind** or **DRD** to detect races in non-TM concurrent code:

```sh
# Build an uninstrumented binary (no plugin) for maximum performance
make test_foo_noinst

# Run under Helgrind (slower, more detailed)
valgrind --tool=helgrind ./bin/test_foo_noinst -t 4 -d 1000

# Run under DRD (faster, less overhead)
valgrind --tool=drd ./bin/test_foo_noinst -t 4 -d 1000
```

### What to look for

- **`tm_serialize_lock/unlock`** — Helgrind may report false positives because it doesn't understand the custom mutex semantics. Use DRD's `--exclude-text` or suppression files to filter known-safe patterns.
- **Atomic operations** (`std::atomic`, `__sync_fetch_and_add`) — Helgrind understands these and reports races if they're used inconsistently (some accesses atomic, others plain).
- **Thread-local state** — Accesses to `__thread` globals (e.g., `tm_nested_call_counter`) are thread-private and should NOT race. Helgrind tracks TLS correctly.
- **TM runtime internals** — Skip by adding `--ignore-inner` or using `--tool=drd --ignore-thread-creation`.

### Filtering plugin false positives

The TM runtime uses `sigsetjmp`/`siglongjmp` for transaction aborts, which Helgrind/DRD do not model correctly. Always use the uninstrumented (`_noinst`) binary, not the instrumented one.

### Example: testing a serialize-lock benchmark

```sh
make test_intruder_noinst
valgrind --tool=drd --exclude-text=backends/tm_impl --show-stack-usage=no \
  ./bin/test_intruder_noinst -t 4 -d 2000
```

This excludes all TM backend internals, focusing the race detector on benchmark code only.
