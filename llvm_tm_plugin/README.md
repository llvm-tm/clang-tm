# LLVM TM Instrumentation Plugin

An LLVM `opt` plugin that instruments annotated transactional
memory functions and globals in C/C++ code.

## Layout

```
llvm_tm_plugin/
├── src/                   # Plugin source files
│   ├── TMInstrumentPass.cpp    # Main pass (module + function-level)
│   ├── tm_runtime_hooks.hpp    # Hook function declarations
│   ├── tm_method_instrumentation.hpp  # Function cloning + load/store inst.
│   ├── tm_annotation_utils.hpp       # Annotation detection helpers
│   ├── tm_local_vars.hpp            # Local variable detection
│   ├── tm_call_graph.hpp            # Call graph construction
│   ├── tm_thread_symbols.hpp        # Thread entry point detection
│   ├── tm_thread_guard.hpp          # Thread init/exit guard insertion
│   └── tm_debug.hpp                 # Debug macros
├── test/                  # Transactional test cases
├── runtime/               # Debug TM runtime (for plugin tests)
├── out/                   # Intermediate bitcode
├── bin/                   # Built plugin .so files + test executables
└── tm_pipeline.mk         # Shared build pipeline include
```

## Features

- Instruments functions annotated with `__attribute__((annotate("transaction")))`
  (convenience macro: `TX`).
- Instruments reads/writes to globals annotated with `__attribute__((annotate("tm")))`
  (convenience macro: `TM`).
- Supports 1-, 2-, 4-, 8-byte integer, float, double, and pointer access.
- **Three pipeline variants** (select via `TM_INSTRUMENT_PIPELINE`):
  - `tm-instrument-inline` (default): inlines all clones then instruments post-inline
  - `tm-instrument`: non-inline, clones survive as `NoInline`+`OptimizeNone` (debug-friendly)
  - `tm-instrument-then-inline`: instruments clones individually then inlines
- **BUILD_TYPE=DEBUG** auto-selects `tm-instrument` pipeline with `-O0` post-opt.
- **Smart variable detection**: local (stack) variables are not
  instrumented; only shared globals.
- **STL support**: container internals are cloned through the call
  graph (e.g. `std::map::find`, `std::vector::begin`).
- **Nested transaction support**: thread-local counter handles
  nesting; only outermost calls `tm_begin`/`tm_end`.
- **Retry via sigsetjmp**: optional — can be disabled at compile time.
- **Malloc/free interception**: `malloc`/`free` inside TX functions
  are replaced with `tm_malloc`/`tm_free`.
- **Memory intrinsics**: `memcpy`/`memmove`/`memset` on TM globals
  are replaced with per-byte instrumented loops.
- **Thread init/exit**: `main()` and annotated thread entry points get
  `tm_init`/`tm_exit`/`tm_init_thread`/`tm_exit_thread` calls.
- **PSTATIC_REBUILD**: functions annotated with `"pstatic_rebuild"`
  are called after `tm_init()` to restore pointer-based data structures.

## Build

```sh
cd llvm_tm_plugin
make                # builds bin/libTMInstrument.so (release variant)
make variants       # builds all 5 plugin variants
make test           # build all tests
make run            # build + run all tests
make check          # run tests and verify
```

## Quick Start with `clang-tm`

The **`clang-tm`** script wraps the full pipeline into a single
command that looks and feels like `clang++`:

```sh
# Default: runtime.cpp in current directory
clang-tm -std=c++20 -O3 -pthread -o myapp app.cpp

# Specify runtime by path or bare name (looked up in backends/runtimes/)
clang-tm --runtime SingleGlobalLock_runtime.cpp -o myapp app.cpp
clang-tm --runtime ../backends/runtimes/TL2_runtime.cpp -o myapp app.cpp
```

The runtime source is compiled to LLVM bitcode and merged with the
instrumented IR **before** the final `-O3` pass. This makes `tm_read`,
`tm_write`, and other runtime functions visible to the optimizer so
they can be inlined.  The final binary contains no `tm_read`/`tm_write`
calls — every access is expanded directly.

`clang-tm` also accepts pre-compiled runtime libraries (`.o`, `.a`,
`.so`, `.dylib`, `.bc`) — these are linked as-is without merging.

```sh
# Runtime as a static library
clang-tm --runtime libmyruntime.a -o myapp app.cpp

# Runtime as a shared library
clang-tm --runtime libmyruntime.so -o myapp app.cpp
```

The script auto-discovers the plugin `.so` and backend include paths
relative to its own location.  All unrecognised flags are forwarded
to `clang++` unchanged.

## Install

```sh
cd llvm_tm_plugin
make variants
./install.sh                  # installs to /usr/local/
PREFIX=~/.local ./install.sh  # installs to user-local prefix
```

Installs to `PREFIX/bin/clang-tm` and `PREFIX/lib/clang-tm/{plugin,runtimes,backends}/`.
The `clang-tm` script auto-detects the installed layout, so after
installation `clang-tm` works from any directory:

```sh
clang-tm --runtime SingleGlobalLock_runtime.cpp -o myapp app.cpp
```

Uninstall:

```sh
./llvm_tm_plugin/uninstall.sh
```

Additional flags:
- `--runtime FILE, -r FILE` — path to runtime (`.cpp`/`.cc`/`.cxx`
  source or `.o`/`.a`/`.so`/`.dylib`/`.bc` library; default: `runtime.cpp`)
- `--plugin FILE, -p FILE`  — path to `libTMInstrument.so` (auto-detected)
- `--keep-temps, -k`        — keep intermediate `.bc` files in a temp dir
- `--verbose, -v`           — print each pipeline step
- `--help, -h`              — show usage

## Instrumentation Levels

The plugin supports six compile-time variants, selected by passing
`-D` flags during the plugin build.  Each produces a separate `.so`
that can be used in the pipeline.

| Variant     | `-D` flags                                          | What it does                                                  |
|-------------|------------------------------------------------------|---------------------------------------------------------------|
| `release`   | `-DNDEBUG`                                           | Full instrumentation, no debug output (default)               |
| `no_setjmp` | `-DNDEBUG -DDISABLE_SETJMP`                          | No sigsetjmp/longjmp — no retry on conflict                   |
| `no_rw`     | `-DNDEBUG -DDISABLE_TM_READ_WRITE`                   | No tm_read/tm_write — keep setjmp + malloc interposition      |
| `no_malloc` | `-DNDEBUG -DDISABLE_MALLOC_FREE`                     | No malloc/free replacement — keep setjmp + r/w                |
| `minimal`   | `-DNDEBUG -DDISABLE_SETJMP -DDISABLE_TM_READ_WRITE -DDISABLE_MALLOC_FREE` | Transaction boundaries only — no data access instrumentation |
| `debug`     | *(none)*                                             | Full instrumentation with verbose debug output to stderr      |

### What each flag controls

| Flag                      | Effect in the plugin                                                  |
|---------------------------|-----------------------------------------------------------------------|
| `DISABLE_SETJMP`          | Skip `sigsetjmp`/`tm_set_jmpbuf` injection.  Transaction entry is    |
|                           | just `tm_begin` + counter increment.  No retry path.                  |
| `DISABLE_TM_READ_WRITE`   | Skip all `tm_read_*`/`tm_write_*` calls for loads/stores and         |
|                           | memory intrinsics.  Cloned functions still exist but are not          |
|                           | instrumented.  Useful for runtimes that don't need per-access hooks   |
|                           | (e.g. SingleGlobalLock).                                              |
| `DISABLE_MALLOC_FREE`     | Skip `malloc`→`tm_malloc` and `free`→`tm_free` replacement.          |

### Building with a specific variant

```sh
# Use the no_setjmp plugin in a benchmark Makefile:
make -C benchmarks/test/bank \
    TM_PLUGIN=../../llvm_tm_plugin/bin/libTMInstrument_no_setjmp.so \
    bank_singlelock
```

### When to use each variant

| Variant     | Best for                                                               |
|-------------|------------------------------------------------------------------------|
| `release`   | Full-featured benchmarking; any STM backend.                           |
| `no_setjmp` | Backends that never abort (SingleGlobalLock).  Simplifies IR,          |
|             | slightly faster compile and link.                                      |
| `no_rw`     | Measuring instrumentation overhead.  Compares the cost of setjmp +     |
|             | malloc interposition alone vs. full per-access instrumentation.        |
| `no_malloc` | Benchmarks that don't allocate inside transactions.  Avoids the        |
|             | overhead of routing through `tm_malloc`.                               |
| `minimal`   | Assessing the absolute minimum overhead of the plugin.                 |
|             | Only `tm_begin`/`tm_end` are injected; no data access hooks at all.   |

## Verification

```sh
# Verify annotations are detected in a specific bitcode file
./test/verify_annotations.sh out/myapp.bc out/myapp.plugin.log

# Check for "TM-annotated symbols" and "Instrumenting transaction function"
# in the log — these must be present for correct instrumentation.
```

## Using `tm_pipeline.mk`

Include the shared pipeline file in your Makefile:

```makefile
include path/to/tm_pipeline.mk
$(eval $(call tm_define_rules))
$(eval $(call tm_target, myapp_singlelock, myapp.cpp, singlelock))
```

The pipeline provides:

| Function / Variable              | Purpose                                            |
|----------------------------------|----------------------------------------------------|
| `$(eval $(call tm_define_rules))`| Creates `%.bc` → `%.instr.bc` → `%.opt.bc` rules   |
| `$(eval $(call tm_target, name, src, backend))` | Creates a complete binary target      |
| `$(call tm_compile_ir,src,out)`  | Compile `.cpp` → `.bc`                              |
| `$(call tm_instrument,in,out)`   | Run the plugin on `.bc` → `.instr.bc`               |
| `$(call tm_optimize,in,out)`     | `opt -O3` on `.instr.bc` → `.opt.bc`               |
| `$(call tm_link,in,backend,out)` | Link `.opt.bc` + runtime → binary                   |
| `TM_PLUGIN`                      | Path to the plugin `.so` (override to use variants) |
| `TM_INSTRUMENT_PIPELINE`         | Pipeline: `tm-instrument-inline` (default), `tm-instrument`, or `tm-instrument-then-inline` |
| `TM_OPT_LEVEL`                   | Post-instrumentation opt level: `-O3` (default), `-O0` |
| `TM_LINK_OPT`                    | Link optimization: `-O1` (default), `-O0 -g` (debug) |
| `BUILD_TYPE`                     | `RELEASE` (default) or `DEBUG` — sets pipeline, opt, and link together |

Available backends: `singlelock`, `norec`, `tl2`, `tinystm`,
`swisstm`, `persistentsgl`, `distributedsgl`.

## Pipeline Design

Three pipelines are available (select via `TM_INSTRUMENT_PIPELINE`):

### `tm-instrument-inline` (default)
1. **TMInitInjectPass** — injects `tm_begin`/`tm_end` into TX functions.
2. **AlwaysInlinerPass** — inlines all reachable callees into TX functions.
3. **TMInstrumentInlinePass** — instruments loads/stores/malloc/mem intrinsic inside inlined TX code.
4. **TMStripLifetimePass** — strips `llvm.lifetime.start/end` (LLVM 22 compat).

Best for production: 176 TM ops, maximal optimization. Post-instrumentation `-O3` required.

### `tm-instrument` (non-inline, debug-friendly)
1. **TMGlobalInitPass** — collects globals, builds call graph, clones reachable functions,
   instruments `_tm_clone` functions (loads/stores/malloc/mem intrinsic).
2. **TMInstrumentPass** — injects `tm_begin`/`tm_end`, nested counter, setjmp/retry
   into TX functions.
3. **TMStripLifetimePass** — strips `llvm.lifetime.start/end`.

Clones survive as `NoInline`+`OptimizeNone` — 38 TM ops. Selected by `BUILD_TYPE=DEBUG`.
Breakpoints on individual clones work because they are not inlined.

### `tm-instrument-then-inline` (experimental)
1. **TMGlobalInitThenInlinePass** — clones and instruments each clone individually.
2. **AlwaysInlinerPass** — inlines instrumented clones into TX functions.
3. **TMInstrumentPass** — instruments remaining loads/stores in TX functions.
4. **TMStripLifetimePass** — strips `llvm.lifetime.start/end`.

Produces 204 TM ops (more than `tm-instrument-inline`). Fails identically to inline
pipeline on some tests — root cause under investigation.

### Shared utilities (all pipelines)
- **`TMStripLifetimePass`**: strips `llvm.lifetime.start`/`end` calls everywhere (LLVM 22 compat).
- Module-level work (symbol tables, global init) is done once.
- Function passes run per-function, modifying IR of individual transaction functions independently.

## Annotation Reference

```cpp
#define TM  __attribute__((annotate("tm")))           // shared global
#define TX  __attribute__((annotate("transaction"), noinline))  // transaction
#define THREAD __attribute__((annotate("thread"), noinline))    // thread entry
#define MAIN __attribute__((annotate("main"), noinline))        // main alternative
#define PSTATIC_REBUILD __attribute__((annotate("pstatic_rebuild")))  // restore fn
```

## Test Cases

| Test                       | What it verifies                                  |
|----------------------------|---------------------------------------------------|
| `simple`                   | Basic load/store instrumentation                  |
| `types`                    | All integer/float/pointer type support            |
| `memtest`                  | Memory intrinsic replacement (memcpy/memset)      |
| `nested`                   | Nested transaction counter logic                  |
| `threads`                  | Thread init/exit instrumentation                  |
| `retry`                    | sigsetjmp/longjmp retry mechanism                 |
| `persist`                  | tm_init/tm_exit with state save/restore           |
| `test_stl_containers`      | std::vector/map/string inside transactions        |
| `test_stl_primitive`       | Primitive arrays in STL containers                |
| `annotation_detect`        | Annotation detection                              |
| `local_containers_test`    | Local vs. shared variable detection               |
| `custom_class_test`        | Custom class with TM-annotated members            |
