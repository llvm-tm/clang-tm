# PersistentSGL and DistributedSGL — Showcase Backends

These two backends are **not** production STM implementations.
They are minimal, didactic backends written to demonstrate two
capabilities of the LLVM TM plugin:

1. **Persistent transactions** via mmap'd storage
2. **Distributed transactions** via inter-process shared memory

Neither backend performs read-set or write-set logging.  Both use
a single global lock (`std::mutex`) to serialise all transactions.
The value they provide is *conceptual*: they show how the plugin's
`tm_init`/`tm_exit`/`tm_malloc` hooks can be wired up to support
use cases beyond classic STM.

---

## PersistentSGL

### What it does

- Allocates a 64 MB bump allocator inside a file (`tm_persist.bin`)
  mapped at a fixed virtual address (`0x600000000000`).
- `tm_begin`/`tm_end` serialise via a global mutex.
- `tm_malloc` returns bump-allocated addresses inside the mmap region.
- `tm_exit` serialises every `TM`-annotated global to the file before
  unmapping.
- On the next run, `tm_init` re-maps the file and re-loads the symbol
  state.

### Limitations

- **No recycling**: the bump allocator never frees memory.  Works for
  workloads that allocate once and read repeatedly.
- **Fixed address**: requires `0x600000000000` to be free (falls back
  to ASLR if `MAP_FIXED` fails).
- **Pointer-based containers** (e.g. `std::map`): tree nodes contain
  raw pointers that become invalid when the mmap is re-mapped.  The
  `PSTATIC_REBUILD` annotation exists for this: the user writes a
  rebuild function annotated with `__attribute__((annotate("pstatic_rebuild")))`
  that reconstructs the container from key-value arrays after `tm_init`.

### What it demonstrates

- How the plugin's `tm_init`/`tm_exit` hooks connect to persistent
  storage.
- How `tm_malloc`/`tm_free` backed by mmap enable persistent
  allocations.
- The `PSTATIC_REBUILD` annotation mechanism.
- Why a `g_in_tx` flag lets `operator new` dispatch to `tm_malloc`
  without changing signatures.

---

## DistributedSGL

### What it does

- N processes share state through an mmap'd file.
- `TM_NPROCESSES=N` environment variable sets the expected count.
- `tm_init` barrier: waits until N processes have initialised.
- `tm_begin` acquires a spinlock (PREPARE phase), then syncs TM
  symbol data FROM the shared mmap.
- `tm_end` syncs TM symbol data TO the shared mmap, `msync`s,
  increments the epoch, and releases the lock (COMMIT phase).
- `tm_exit` decrements the process count.

### Limitations

- **No fault tolerance**: if a process crashes mid-transaction, the
  spinlock is never released.
- **Full-state sync**: every transaction copies ALL TM globals,
  regardless of which were actually modified.
- **Sequential commits**: the global lock limits throughput to
  1 transaction at a time across all processes.

### What it demonstrates

- How the plugin's hooks can implement a two-phase commit protocol.
- Inter-process shared-memory state via mmap.
- The `RelPtr<T>` relative-pointer class for ASLR-safe shared data.
- Atomic synchronisation (`std::atomic`) on shared memory between
  independent processes.

---

## Comparison

| Aspect              | PersistentSGL                  | DistributedSGL                  |
|---------------------|--------------------------------|---------------------------------|
| Persistence         | Data survives process restart  | Data vanishes with mmap file    |
| Processes           | Single                         | Multiple (N)                    |
| Synchronisation     | `std::mutex`                   | Spinlock over shared memory     |
| Allocator           | 64 MB bump (inside mmap)       | System malloc                   |
| Malloc routing      | `tm_malloc` → mmap,            | Not overridden                  |
|                     | `operator new` → mmap in TX    |                                 |
| TM data sync        | File save on `tm_exit`         | On every `tm_begin`/`tm_end`    |

## When to Use These

- **Learning**: read the source to understand how the plugin hooks map
  to runtime functionality.
- **Experimentation**: modify them to add your own allocator, locking
  scheme, or serialisation format.
- **Assessment**: compare throughput against an unmodified
  SingleGlobalLock to measure the overhead of persistence/sync.

For production use, replace these with proper STM backends (TL2,
NOrec, TinySTM) or with a real persistent TM (see `research_notes.md`).
