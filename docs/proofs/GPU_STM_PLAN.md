# GPU STM Backend Plan

## State of the Art

### No commercial GPU HTM

NVIDIA and AMD do not expose hardware transactional memory to programmers.
All GPU TM is software-based (STM). Academic proposals for GPU HTM exist but
are simulation-only (Kilo TM, WarpTM — both use simulation frameworks, no
silicon).

### GPU STM algorithms (2014–2025)

| System | Year | Algorithm | Key technique |
|--------|------|-----------|---------------|
| GPU-STM | 2014 | Lock-based, encounter-time | Hierarchical validation (time + value); lock-sorting prevents deadlock |
| Lightweight GPU STM | 2014 | Eager/pessimistic/invisible | 3 variants (ESTM, PSTM, ISTM); warp-level retry |
| PR-STM | 2015 | Lock-based, commit-time | Static thread priorities for lock stealing; 32-bit lock word encodes priority+version+locked |
| CSMV | 2022 | Multi-versioned | Client-server commit: server warp validates, clients batch writes; K-version read sets |
| OFG-STM | 2024 | Obstruction-free | Locator-based ownership; warp-level GC via Cooperative Groups |
| AccelerateSTM | 2025 | Obstruction-free | Improved locator GC; per-warp activation; fastest vs 3 prior systems |
| BifurKTM | 2021 | Distributed + approximate | KoSTM (K-opaque reads, relaxed consistency); cluster-scale |

### GPU-specific challenges

1. **Warp execution (SIMT)**: 32 threads share instruction counter. Branch
   divergence serializes paths. Cannot spin-wait on a lock (divergence kills
   warp throughput). Solutions: lock-sorting (GPU-STM), priority-stealing
   (PR-STM), retry with warp-mask (Lightweight GPU-STM).
2. **Memory coalescing**: 32 threads' accesses to consecutive addresses merge
   into one wide transaction. Random access patterns kill bandwidth.
3. **No thread migration**: A GPU thread is pinned to its warp slot. No
   `siglongjmp` — retry via loop + warp-mask.
4. **Limited shared memory**: 48–164 KB per block. Read-set/write-set must
   fit or spill to global memory.
5. **No TLS**: Thread-local state via register arrays or per-thread global
   memory slots.

## Architecture for backends

### Design: CUDA C++ library with C API

The GPU backend is a CUDA C++ library compiled with `nvcc`. It exposes a
C API matching the hooks system (`TMRealHooks`).

```
backends/tm_impl/
  gpu_stm/              -- PR-STM (Shen et al. 2015)
    include/            -- public C API header (gpu_stm_api.h)
    cpu/                -- CPU fallback (gpu_stm_cpu_runtime.cpp, pr_stm_cpu.cpp)
    cuda/               -- CUDA kernel (pr_stm_kernel.cuh, pr_stm_runtime.cu)
    tests/              -- smoke test (test_pr_stm.cpp)
  csmv/                 -- CSMV (Nunes et al. 2022) — separate backend dir
    include/            -- C API header (csmv_api.h)
    cpu/                -- CPU fallback (csmv_cpu_runtime.cpp)
    gpu/                -- CUDA kernel + batch executor (csmv_kernel.cu, *.hpp)
    CMakeLists.txt      -- CUDA-enabled build
```

### Host-device split

```
┌─────────────────────────────────────────────┐
│                Host (CPU)                   │
│  tm_init() → cudaSetDevice, alloc tables    │
│  tm_begin() → launch kernel or set flag     │
│  tm_read/write → enqueue in command buffer  │
│  tm_commit() → launch commit kernel          │
│  tm_exit() → cudaDeviceSynchronize, free    │
└──────────────┬──────────────────────────────┘
               │ cudaMemcpy / kernel launch
┌──────────────▼──────────────────────────────┐
│              Device (GPU)                   │
│  Kernel: warp-level TM processing           │
│  Global lock table in device memory         │
│  Read-set/write-set in shared memory        │
│  Cooperative Groups for warp sync           │
└─────────────────────────────────────────────┘
```

### C API (matching TMRealHooks)

```c
// All hooks are device-callable or host-callable dispatch to kernels
void  gpu_tm_init(void);
void  gpu_tm_exit(void);
void  gpu_tm_init_thread(void);
void  gpu_tm_exit_thread(void);
void  gpu_tm_begin(void);
void  gpu_tm_end(void);
int   gpu_tm_commit(void);  // 0=abort, 1=commit
void *gpu_tm_malloc(size_t sz);
void  gpu_tm_free(void *p);
uint32_t gpu_tm_read_i4(uint32_t *addr);
void     gpu_tm_write_i4(uint32_t *addr, uint32_t val);
// ... other types
```

### Challenge: host-device boundary

The current test harness (`test_tx.cpp`, `bank.cpp`) runs on CPU and calls
`tm_read_i4` etc. from CPU threads. A GPU backend requires:

**Option A: Kernel-launch-per-transaction**
- `tm_begin()` records transaction parameters
- `tm_read()` / `tm_write()` buffer in host memory
- `tm_commit()` launches a CUDA kernel that executes the transaction on GPU
- Pro: clean separation, no CUDA in test harness
- Con: kernel launch overhead per transaction (~5–10 µs), only viable for
  large transactions

**Option B: Persistent kernel**
- `tm_init()` launches a persistent kernel that stays resident on GPU
- `tm_begin()` pushes a transaction descriptor to the GPU via circular buffer
- GPU warps continuously drain the buffer, execute transactions, write results
- Pro: amortizes launch overhead
- Con: complex synchronization, polling vs interrupts

**Option C: Unified memory model**
- Use `cudaMallocManaged` for TM region
- Host threads access GPU memory directly via unified memory
- Pages migrate on access — dramatic slowdown for fine-grained TM ops
- Not viable for performance, but simplest for correctness prototyping

**Recommendation: Start with Option A (launch-per-tx) for correctness,**
**then Option B (persistent kernel) for performance.**

## PR-STM backend (first target)

PR-STM (Shen et al., 2015) is the best first target because:
- Published, well-described algorithm with pseudocode
- Lock-based (familiar from CPU STMs like TL2/TinySTM)
- Priority-based contention management avoids warp-divergence spin
- 32-bit lock word is GPU-friendly (single atomicCAS per lock)
- Source code not publicly available — must implement from paper

### Transaction flow (per GPU thread)

```
txStart:
  clear read-set, write-set, lock-set (shared memory arrays)
  read global clock → start_time

txRead(addr):
  if addr in write-set: return cached value
  version := lock_table[hash(addr)] & VERSION_MASK
  if LOCKED(lock_table[hash(addr)]): abort
  add (addr, version) to read-set
  return *addr

txWrite(addr, val):
  add (addr, val) to write-set

txValidate:
  for each (addr, ver) in read-set:
    if lock_table[hash(addr)] & VERSION_MASK != ver: abort
  for each addr in write-set:
    prelock using priority CAS
    if prelock fails due to higher-priority holder: abort

txCommit:
  for each addr in write-set:
    lock using atomicCAS (steal-protected)
  write values to global memory
  __threadfence()
  increment lock_table version bits
  release locks
```

### Warp-level optimizations

- **Coalesced lock table reads**: Lock table is an array of 32-bit words in
  global memory. Threads in a warp read consecutive entries → coalesced.
- **Warp-level validation**: All 32 threads in a warp validate together;
  `__any_sync()` detects if any thread aborted.
- **Batched commit**: All threads in a warp that pass validation commit
  together; `__syncwarp()` ensures ordering.

### Files to create

```
backends/tm_impl/gpu_stm/
  CMakeLists.txt           -- CUDA-enabled build
  include/
    gpu_stm_api.h          -- C API (TMRealHooks-compatible)
  common/
    warp_utils.cuh         -- warp-level ballot, sync, mask helpers
    lock_table.cuh         -- 32-bit lock word manipulation
    global_clock.cuh       -- device-side atomic clock
  pr_stm/
    pr_stm.cuh             -- PR-STM algorithm (device functions)
    pr_stm_runtime.cpp     -- Host-side dispatcher (Option A: launch-per-tx)
    pr_stm.cu              -- Kernel entry points
  csmv/                    -- (future)
```

## Verification strategy

### Unit tests (CUDA)
- Single-thread warp: begin, read, write, commit
- Multi-thread warp: concurrent transfers, verify atomicity
- Abort path: trigger conflict, verify rollback

### Integration with existing test suite
- `test_tx.cpp` adapted: wrap each transaction in a kernel launch
- `test_ds.cpp` adapted: data structure ops on GPU-resident data
- `bank` benchmark: full GPU port

### Correctness invariants
Same TLA+ models apply — GPU STM algorithms follow the same OCC/lock-based
protocols as CPU backends. The PR-STM models can share the `TL2.tla` or
`TinySTM_WBCTL.tla` spec with modifications for priority-stealing and the
32-bit lock encoding.

## Priority and timeline

| Phase | What | Effort |
|-------|------|--------|
| P0 | PR-STM implementation (CUDA C++, Option A launch-per-tx) | ~2 weeks |
| P1 | PR-STM + persistent kernel (Option B) for low-overhead | ~2 weeks |
| P2 | CSMV backend (multi-versioned, client-server) | ~3 weeks |
| P3 | TLA+ model for PR-STM (priority-stealing + 32-bit lock) | ~1 week |
| P4 | Benchmark port (bank, fuzz_counter, STAMP subset) | ~2 weeks |
| P5 | AccelerateSTM backend (obstruction-free) | ~3 weeks |
| P6 | Integration with host-side hooks system + CI | ~1 week |

## References

1. Xu et al., "Software Transactional Memory for GPU Architectures", CGO 2014
2. Shen et al., "PR-STM: Priority Rule Based Software Transactions for the GPU", 2015
3. Nunes et al., "CSMV: A Highly Scalable Multi-Versioned STM for GPUs", IPDPS 2022
4. Perlin et al., "OFG-STM: TM for GPUs based on Obstruction-Free STM", SBLP 2024
5. Perlin et al., "Obstruction-Free STM for GPUs", SBAC-PAD 2025
6. Fung et al., "Energy Efficient GPU TM via Space-Time Optimizations", MICRO 2013 (WarpTM)
7. Chen & Peng, "Accelerating GPU HTM with Snapshot Isolation", ISCA 2017
