# Software Requirements

This document lists the software needed to build and run the TM API C++ project
on a new machine (including remote machines accessed via SSH).

## Required

| Tool | Version | Notes |
|------|---------|-------|
| LLVM | 22+ | Default and pinned development version: **22**. `opt`, `llvm-link`, `llvm-dis` |
| Clang / clang++ | matching LLVM version | Must support `-emit-llvm` and `-load-pass-plugin` |
| Python | 3.8+ | For `tm-resolve-opaque.py` (opaque symbol resolution); `pip install -r requirements.txt` for optional plotting/analysis scripts |
| GNU Make | 4+ | For the build system |
| pthreads | — | System library (usually built-in) |
| C++ Standard Library | — | Must support C++20 (`std::atomic`, `thread_local`) |
| Bash | 4+ | For `clang-tm` and other scripts |
| coreutils | — | macOS: `brew install coreutils` (provides `gtimeout`) |

## Selecting LLVM versions

The default toolchain remains LLVM 22. The Makefile, `clang-tm`, and
`tools/check-requirements.sh` honor an optional `LLVM_VERSION` environment
variable or make variable (for example, `LLVM_VERSION=23 make plugin`). When
`LLVM_VERSION` is set explicitly, discovery must find that LLVM major version;
otherwise it may fall back to any LLVM 22+ installation. CI exercises LLVM 22
as required and LLVM 23 as a non-blocking compatibility matrix entry.

## Installation by Platform

### Ubuntu / Debian

```sh
sudo wget -q -O /etc/apt/trusted.gpg.d/llvm-snapshot.asc https://apt.llvm.org/llvm-snapshot.gpg.key
sudo add-apt-repository "deb https://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-22 main"
sudo apt update
sudo apt install -y llvm-22-dev clang-22 lld make
```

### macOS (Homebrew)

```sh
brew install llvm coreutils gnu-time make
```

Ensure the LLVM `bin/` directory is on `PATH` (usually
`/opt/homebrew/opt/llvm/bin/` on ARM Mac, `/usr/local/opt/llvm/bin/` on Intel).

### Verify Installation

Run `./tools/check-requirements.sh` in the project root to verify all tools are
available at compatible versions.

Install optional Python dependencies for plotting/analysis tools with:

```sh
python3 -m pip install -r requirements.txt
```

To confirm the full toolchain (plugin, C++ backends, Rust) works end-to-end,
run the ~60s smoke test:

```sh
make check-fast
```

## Project Structure After Copying to Remote Machine

```
/path/to/project/
├── plugin/
│   ├── clang-tm              # Main compilation script
│   ├── tm-resolve-opaque.py  # Opaque symbol resolution tool
│   ├── bin/libTMInstrument.so  # Plugin binary (must be built first)
│   ├── passes/               # Plugin source (LLVM passes)
│   ├── runtime/              # Generic TM runtime
│   └── tm_pipeline.mk        # Build system include
├── backends/
│   └── tm_impl/              # STM runtime implementations (one subdir per backend)
└── benchmarks/
    ├── cpp/bank/             # Example benchmark (explicit C++ API)
    └── plugin/bank/          # Example benchmark (plugin-instrumented)
```

## Quick Setup on New Machine

### One-liner install (requires LLVM/Clang 22+)

```sh
curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/tools/bootstrap-install.sh | bash
```

### Or from a local clone

```sh
git clone https://github.com/llvm-tm/clang-tm.git
cd clang-tm
make plugin
./tools/install-plugin.sh
```

### Build and run a benchmark

```sh
# Plugin-instrumented benchmark
cd benchmarks/plugin/bank
make bank_singlelock
./bin/bank_singlelock -t 4 -d 5000

# Explicit C++ API benchmark (no plugin needed)
make -C benchmarks/cpp BACKEND=NOREC bin/bank
./benchmarks/cpp/bin/bank -t 2 -d 1000 --test
```

> **Note (Linux):** `benchmarks/cpp` links statically by default. Pass
> `STATIC=0` to link dynamically instead (faster builds):
> `make -C benchmarks/cpp BACKEND=NOREC STATIC=0 bin/bank`.

## Optional: gem5 validation

The optional gem5 checkout under `gem5_sim/gem5` is large: expect a multi-GB
clone plus a multi-GB build tree. Only run `make gem5` when you need gem5
experiments; `make check-fast` never clones or builds gem5.

**Known limitation — multi-core Ruby livelock:** the Ruby HTM path
(`gem5_sim/configs/x86-se-bank.py`, `MESI_Three_Level_HTM`) hangs once 2+ cores
are active (an upstream `MESI_Three_Level` coherence livelock, unrelated to the
TSX patches). Keep `--threads 1` for reliable Ruby runs, pass `--max-ticks` to
cap multi-core runs (applied automatically by `scripts/run_gem5_sweep.py` via
`--mt-ticks`), or use the non-Ruby `gem5_sim/configs/x86-se-bank-classic.py`
for unbounded multi-core timing. See
[`gem5_sim/docs/x86-tsx-validation.md`](../gem5_sim/docs/x86-tsx-validation.md)
("Known limitations").
