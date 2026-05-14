# TM API C++ — LLVM Transactional Memory Plugin

An LLVM compiler plugin that automatically instruments C++ code for Software Transactional Memory (STM). Uses source-code annotations (`__attribute__((annotate(...)))`) to mark global variables and functions as transactional.

## Quick Start

```bash
# Build the LLVM plugin and all benchmarks
make all

# Run the bank benchmark with SingleGlobalLock (fastest backend)
make test_run
```

## Architecture

```
tm_api_cpp/
├── llvm_tm_plugin/       # LLVM plugin (the core)
│   ├── src/              # Source: TMInstrumentPass.cpp + headers
│   ├── test/             # Test programs (types, nested, threads, etc.)
│   ├── runtime/          # Basic debug runtime (tm_runtime.cpp)
│   └── tm_pipeline.mk    # Shared compilation pipeline (4 steps)
├── backends/             # STM runtime backends
│   ├── runtimes/         # Runtime wrappers (one per backend)
│   ├── TinySTM/          # Write-back CTL/ETL + Write-through
│   ├── TL2/              # Transactional Locking 2
│   ├── NOrec/            # Lazy value-based validation
│   ├── SwissTM/          # Hybrid lazy/pessimistic
│   └── tests/            # Backend unit tests
├── benchmarks/           # TM benchmarks
│   ├── test/bank/        # Bank (money transfer, correctness check)
│   ├── datastructures/   # AVL, RB tree, hashmap, list, set
│   ├── STMbench7/        # Complex CAD/CAM graph benchmark
│   ├── STAMP/            # Stanford TM benchmark suite
│   ├── TPCC/             # OLTP benchmark
│   ├── YCSB/             # Cloud serving benchmark
│   └── EigenBench/       # Synthetic TM exploration
└── docs/                 # Design docs and reports
```

## Programming Model

Mark globals and functions with annotations:

```cpp
#define TM  __attribute__((annotate("tm")))
#define TX  __attribute__((annotate("transaction"), noinline))

TM int global_counter = 0;

TX void increment() {
    global_counter++;
}
```

The plugin:
1. Instruments `TM` globals with `tm_read_*`/`tm_write_*` calls
2. Wraps `TX` functions in transaction begin/commit + retry logic
3. Clones non-TX functions reachable from TX functions (with `_tm_clone` suffix)
4. Redirects calls within TX code to instrumented clones

## Annotations

| Annotation | Purpose |
|---|---|
| `TM` | Variable is shared and needs TM tracking |
| `TX` | Function executes as a transaction |
| `THREAD` | Function is a thread entry point (gets tm_init_thread) |
| `MAIN` | Entry point (gets tm_init/tm_exit) |

## Backends

| Backend | Build Target | Strategy | 2T Correct | 4T Correct |
|---|---|---|---|---|
| SingleGlobalLock | `_singlelock` | Global mutex | ✅ | ✅ |
| NOrec | `_norec` | Value-based validation | ✅ | ✅ |
| TL2 | `_tl2` | Commit-time locking | ✅ | ❌ (money off by ±3) |
| TinySTM (wbctl) | `_tinystm` | Write-back CTL | ⚠️ Slow | ⚠️ Slow |
| SwissTM | `_swiss` | Lazy commit-time | ⚠️ Slow | ⚠️ Slow |

## Build Targets

```bash
make plugin          # Build libTMInstrument.so
make benchmarks     # Build all benchmarks
make tests          # Build plugin test suite
make test_run       # Quick smoke test
make clean          # Clean everything
```

## Writing a Transactional Program

```cpp
#include <cstdio>

#define TM  __attribute__((annotate("tm")))
#define TX  __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM int counter = 0;

TX void increment() {
    counter++;
}

MAIN int main() {
    increment();
    printf("counter = %d\n", counter);
    return 0;
}
```

Compile with the pipeline:
```bash
clang++ -O3 -fno-inline -emit-llvm -c prog.cpp -o prog.bc
opt -load-pass-plugin=libTMInstrument.so -passes="tm-instrument" prog.bc -o prog.instr.bc
opt -O3 prog.instr.bc -o prog.opt.bc
clang++ prog.opt.bc backends/runtimes/NOrec_runtime.cpp -o prog
```

## References

- Zardoshti et al., "Simplifying Transactional Memory Support in C++", ACM TACO 2019
- STMbench7: Guerraoui, Kapalka, Vitek, EuroSys 2007
- TinySTM: Felber, Fetzer, Riegel, PPoPP 2008
- TL2: Dice, Shalev, Shavit, DISC 2006
- NOrec: Dalessandro, Spear, Scott, PPoPP 2010
