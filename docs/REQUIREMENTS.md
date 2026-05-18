# Software Requirements

This document lists the software needed to build and run the TM API C++ project
on a new machine (including remote machines accessed via SSH).

## Required

| Tool | Version | Notes |
|------|---------|-------|
| LLVM | 22+ | Including `opt`, `llvm-link`, `llvm-dis` |
| Clang / clang++ | matching LLVM version | Must support `-emit-llvm` and `-load-pass-plugin` |
| Python | 3.8+ | For `tm-resolve-opaque.py` (opaque symbol resolution) |
| GNU Make | 4+ | For the build system |
| pthreads | — | System library (usually built-in) |
| C++ Standard Library | — | Must support C++20 (`std::atomic`, `thread_local`) |
| Bash | 4+ | For `clang-tm` and other scripts |
| coreutils | — | macOS: `brew install coreutils` (provides `gtimeout`) |

## Installation by Platform

### Ubuntu / Debian

```sh
wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
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

Run `./check-requirements.sh` in the project root to verify all tools are
available at compatible versions.

## Project Structure After Copying to Remote Machine

```
/path/to/project/
├── llvm_tm_plugin/
│   ├── clang-tm              # Main compilation script
│   ├── tm-resolve-opaque.py  # Opaque symbol resolution tool
│   ├── bin/libTMInstrument.so  # Plugin binary (must be built first)
│   ├── src/                  # Plugin source (plugin + opaque_safe_table.hpp)
│   ├── runtime/              # Generic TM runtime
│   └── tm_pipeline.mk        # Build system include
├── backends/
│   ├── runtimes/             # STM runtime implementations
│   ├── include/              # Backend headers
│   ├── SwissTM/              # SwissTM backend
│   ├── TinySTM/              # TinySTM backend
│   ├── TL2/                  # TL2 backend
│   └── NOrec/                # NOrec backend
└── benchmarks/
    └── test/bank/            # Example benchmark
```

## Quick Setup on New Machine

### One-liner install (requires LLVM/Clang 22+)

```sh
curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/llvm_tm_plugin/bootstrap-install.sh | bash
```

### Or from a local clone

```sh
git clone https://github.com/llvm-tm/clang-tm.git
cd clang-tm/llvm_tm_plugin
make variants
./install.sh
```

### Build and run a benchmark

```sh
clang-tm -std=c++20 -O3 -pthread --runtime SingleGlobalLock_runtime.cpp \
    -o bank bank.cpp
./bank -d 3000 -t 4
```
