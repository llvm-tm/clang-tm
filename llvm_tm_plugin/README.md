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
- **Transitive instrumentation**: all functions reachable from TX
  functions are automatically cloned and instrumented.
- **Function versioning**: instrumented (`_tm_clone`) versions coexist
  with originals; non-TX code paths have zero overhead.
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

Available backends: `singlelock`, `norec`, `tl2`, `tinystm`,
`swisstm`, `persistentsgl`, `distributedsgl`.

## Pipelines

The plugin provides three pipelines, selected via `-passes=<name>`:

| Pipeline | Steps | Best for |
|----------|-------|----------|
| `tm-instrument` | Clone → instrument clones → instrument TX body | Debugging (`-O0`/`-g`); simplest IR |
| `tm-instrument-inline` | Clone with `alwaysinline` → inline into TX body → instrument | Maximum performance; requires `-O1+` |
| `tm-instrument-then-inline` | Clone + redirect → instrument TX body → inline → post-inline instrument | Cleaner separation; no pre-inline instrumentation of clones |

Default in `tm_pipeline.mk`: `tm-instrument-inline`.

## Two-Pass Design

Each pipeline operates as multiple LLVM passes:

### Module-level work (runs once per module)
- Collects `TM`-annotated globals and creates symbol tables.
- Builds the TX-reachable call graph.
- Clones all reachable non-TX functions.
- Redirects calls in TX functions to clones.
- Injects `tm_init`/`tm_exit` into `main()` and thread entry points.
- Calls `"pstatic_rebuild"` functions after `tm_init`.

### Function-level passes (per function)
- Inserts nested transaction counter logic.
- (Optional) Inserts `sigsetjmp`/`tm_set_jmpbuf` for retry.
- Replaces loads/stores with `tm_read_*`/`tm_write_*`.
- Replaces `malloc`/`free` with `tm_malloc`/`tm_free`.
- Replaces `memcpy`/`memmove`/`memset` on TM data.

The module/function split is required because:
- Module-level work (symbol tables, global init) must happen once.
- Function-level passes run per-function; they can modify the IR of
  individual transaction functions independently.

### Thread entry auto-detection

In addition to the `THREAD` annotation, the plugin auto-detects common
thread-entry patterns by symbol name (defined in `tm_thread_symbols.hpp`):

| Pattern | Examples |
|---------|----------|
| `pthread_create` | `pthread_create`, `start_thread` |
| `std::thread` | `_State_impl`, `execute_native_thread_routine` |
| OpenMP | `__kmp_launch_thread` |
| TBB | `tbb::internal::start_thread` |
| `clone` | `clone`, `fork_by_syscall_clone` |

Any function matching one of these patterns OR annotated with `THREAD`
gets `tm_init_thread`/`tm_exit_thread` injected.

## `tm_local` Annotation

Every load/store inside a TX function is instrumented by default. For
thread-private stack variables that never escape the function, add
`tm_local` to skip instrumentation and reduce overhead:

```cpp
#define TM_LOCAL __attribute__((annotate("tm_local")))

TX void worker() {
    TM_LOCAL int i;
    for (i = 0; i < 1000; i++)
        g_shared++;   // still instrumented
}
```

Only stack variables (allocas). Pointers to shared memory must NOT be
marked `tm_local`.

## Nesting Mechanism

When a TX function calls another TX function, the plugin flattens the
nesting using a thread-local counter (`tm_nested_call_counter`):

- **Outermost call**: counter 0→1, calls `tm_begin`, runs body, calls `tm_end`
- **Nested call**: counter 1→2→..., skips `tm_begin`/`tm_end` entirely
- **On abort** (`siglongjmp`): execution restarts from the outermost
  `sigsetjmp` point
- The runtime's `tm_begin`/`tm_end` are **only ever called** as outermost;
  they unconditionally execute the full begin/commit logic

## Annotation Reference

```cpp
#define TM  __attribute__((annotate("tm")))           // shared global
#define TX  __attribute__((annotate("transaction"), noinline))  // transaction
#define THREAD __attribute__((annotate("thread"), noinline))    // thread entry
#define MAIN __attribute__((annotate("main"), noinline))        // main alternative
#define TM_LOCAL __attribute__((annotate("tm_local")))          // private local
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
