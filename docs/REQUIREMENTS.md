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

Run `./tools/check-requirements.sh` in the project root to verify all tools are
available at compatible versions.

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
# Build a plugin-instrumented benchmark
cd benchmarks/plugin/bank
make bank_singlelock
./bin/bank_singlelock -t 4 -d 5000
```
