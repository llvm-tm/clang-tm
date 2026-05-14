# STM Backends

This directory contains the Software Transactional Memory backends. Each backend implements the `tm_read_*`/`tm_write_*` hooks that the LLVM plugin calls.

## Architecture

```
backends/
├── runtimes/             # Runtime wrappers (one per backend)
│   ├── NOrec_runtime.cpp
│   ├── SingleGlobalLock_runtime.cpp
│   ├── TinySTM_runtime.cpp
│   ├── SwissTM_runtime.cpp
│   ├── tl2_runtime.cpp
│   ├── PersistentSGL_runtime.cpp
│   └── test_runtime.cpp
├── TinySTM/              # Write-back commit-time/encounter-time + write-through
│   ├── tinystm_common.hpp
│   ├── tinystm_globals.hpp
│   ├── tinystm_wbctl.hpp   # Write-back commit-time locking
│   ├── tinystm_wbetl.hpp   # Write-back encounter-time locking
│   └── tinystm_wt.hpp      # Write-through
├── TL2/                   # Transactional Locking 2
│   └── tl2_new.hpp
├── NOrec/                 # Lazy value-based validation STM
│   ├── NOrec.hpp
│   └── NOrec_globals.hpp
├── SwissTM/               # Hybrid lazy/pessimistic
│   └── SwissTM.hpp
└── tm_common.hpp          # Shared type definitions (any_type_t, ValueType, etc.)
```

## Backend Selection

The backend is selected at link time by including the corresponding runtime:

- `-DDESIGN_WBCTL` + `TinySTM_runtime.cpp` + `-Ibackends/TinySTM`: TinySTM write-back CTL
- `tl2_runtime.cpp` + `-Ibackends/TL2`: TL2
- `NOrec_runtime.cpp` + `-Ibackends/NOrec`: NOrec
- `SwissTM_runtime.cpp` + `-Ibackends/SwissTM`: SwissTM

## Runtime API

Each runtime wrapper exports:

| Function | Purpose |
|---|---|
| `tm_init()` / `tm_exit()` | Global init/cleanup |
| `tm_init_thread()` / `tm_exit_thread()` | Per-thread init/cleanup |
| `tm_begin()` / `tm_end()` | Transaction boundaries |
| `tm_read_i{1,2,4,8}()` | Typed reads |
| `tm_write_i{1,2,4,8}()` | Typed writes |
| `tm_read_f{4,8}()` / `tm_write_f{4,8}()` | Float/double |
| `tm_read_ptr()` / `tm_write_ptr()` | Pointer reads/writes |
| `tm_set_jmpbuf()` / `tm_longjmp()` | Transaction abort/retry |

## Comparison

| Backend | Read Overhead | Write Overhead | Complexity | Speed |
|---|---|---|---|---|
| **SingleGlobalLock** | None | None (serial) | Lowest | Fastest |
| **NOrec** | Low (clock check) | Medium (CAS commit) | Low | Fast |
| **TL2** | Medium (version check) | High (lock/validate) | Medium | Fast (2T) |
| **TinySTM** | High (read-set log) | High (write-set + lock) | High | Slow with instrumented code |
| **SwissTM** | High (lazy validate) | High (per-access) | High | Slow |
