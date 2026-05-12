# LLVM TM Instrumentation Plugin

This project contains an LLVM `opt` plugin that instruments annotated transactional functions and TM globals in C/C++ code.

## Layout

- `src/` contains the instrumentation pass source.
- `test/` contains the transactional test cases.
- `runtime/` contains the debug TM runtime implementation.
- `out/` is used for intermediate bitcode artifacts.
- `bin/` contains the built plugin and test executables.

## Features

- Instruments functions annotated with `__attribute__((annotate("transaction")))`.
- Instruments reads and writes to globals annotated with `__attribute__((annotate("tm")))`.
- Supports 1-, 2-, 4-, and 8-byte integer accesses, floats, doubles, pointers, `memset`, and `memcpy`.
- Includes debug runtime behavior for nested transactions using a thread-local counter and `setjmp`.
- **Transitive Instrumentation**: Automatically instruments all functions reachable from transaction functions.
- **Function Versioning**: Creates separate instrumented versions of functions to eliminate overhead in non-transaction contexts.
- **Smart Variable Detection**: Distinguishes between local stack variables (not instrumented) and shared globals (instrumented).
- **STL Support**: Properly instruments container internals through transitive function call tracking.

## Build

From the `llvm_tm_plugin` directory:

```sh
make
```

This builds `bin/libTMInstrument.so`.

## Run

Build all tests and run them:

```sh
make run
```

## Verify

Run the full verification target:

```sh
make check
```

## Notes

- The build uses `../llvm-config-args.sh` for the LLVM compiler and linker flags.
- Test artifacts and binaries are kept under `out/` and `bin/`.

## Implementation Architecture

The plugin implements transitive instrumentation with function versioning:

1. **Call Graph Analysis**: Builds a complete call graph from transaction entry points.
2. **Function Cloning**: Creates `_tm_instrumented` versions of all reachable functions.
3. **Variable Analysis**: Detects local (non-instrumented) vs. shared (instrumented) variables.
4. **Callsite Rewriting**: Redirects transaction-context calls to instrumented versions.
5. **Zero-Cost Abstraction**: Non-transaction code paths use original functions, zero overhead.

See `IMPLEMENTATION_PLAN.md` for detailed phase documentation and test results.
