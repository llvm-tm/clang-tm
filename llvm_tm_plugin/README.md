# LLVM TM Instrumentation Plugin

This project contains an LLVM `opt` plugin that instruments annotated transactional functions and TM globals in C/C++ code.

## Layout

- `src/` contains the instrumentation pass source.
- `test/` contains the transactional test cases.
- `tm_implementation/` contains the debug TM runtime implementation.
- `out/` is used for intermediate bitcode artifacts.
- `bin/` contains the built plugin and test executables.

## Features

- Instruments functions annotated with `__attribute__((annotate("transaction")))`.
- Instruments reads and writes to globals annotated with `__attribute__((annotate("tm")))`.
- Supports 1-, 2-, 4-, and 8-byte integer accesses, floats, doubles, pointers, `memset`, and `memcpy`.
- Includes debug runtime behavior for nested transactions using a thread-local counter and `setjmp`.

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
