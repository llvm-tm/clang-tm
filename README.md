# TM API — C++ Transactional Memory Framework

Multi-paradigm transactional memory framework for C++ (LLVM plugin, explicit C++ API, Rust bindings).

## Structure

```
backends/          — TM runtime implementations & stubs
├── stubs/         — TM-lite compatible pass-through stubs
└── tm_impl/       — 15+ STM/HTM backends (TinySTM, NOrec, TL2, SGL, etc.)
plugin/            — LLVM instrumentation plugin (5-pass Honorio pipeline)
benchmarks/        — Benchmarks: plugin-instrumented, C++ expli, Rust
expli_instr/       — Explicit instrumentation: C++ API & Rust workspace
tests/             — Tests for all components
patches/           — Debug & profiling patches
tools/             — Build/install scripts + stm_bug_tool
docs/              — Documentation
```

## Quick Start

```bash
make plugin               # Build LLVM plugin (libTMInstrument.so)
make tests                # Build plugin tests
make check                # Run plugin tests
make expli-benchmarks     # Build explicit C++ API benchmarks
```

Select a backend: `make plugin BACKEND=tl2`

## Key Features

- **5-pass decomposed pipeline** (Honorio-style): DualPathInfoCollector → TransactionSafeCreation → ReplaceCallInsideTransaction → LoadStoreBarrierInsertion → Cleanup
- **TM-lite pre-processing pass**: lowers `atomic do` blocks to the same pipeline
- **Multi-backend**: pluggable STM backends via extern "C" ABI
- **Multi-paradigm**: LLVM plugin, explicit C++ API, Rust bindings
- **Annotation-driven barrier elision**: `tm_local` qualifier reduces instrumentation overhead
