# AccelTL2 — Implementation notes

TL2 (see `../tl2/`) with the commit-time conflict check optionally
accelerated on a GPU-class device discovered at runtime. This is the
*inverse* of the repo's GPU_STM_CPU/CSMV backends (which simulate
GPU-resident STM on CPU cores): here transactions execute on CPU threads
as usual, and the **read-set validation scan** is offloaded to the
accelerator over shared memory.

## Why TL2 is the right base

- TL2's validation (tl2.hpp `commit()` step 5) is a pure map over the
  read log: one independent guard-table load + compare per entry —
  embarrassingly parallel and side-effect free.
- The shared state it touches is a fixed-size atomic guard table
  (`g_guards`), which can live once in device-visible memory.
- NOrec has nothing to offload (single global version — O(1)
  validation); SGL serializes everything; TinySTM WBCTL interleaves
  validation with per-word lock release; XTM/SPHT are bound to CPU
  instructions.

## Zero-copy design

- `accel_probe.cpp` `dlopen`s `libOpenCL.so.1` (no build-time GPU dep —
  CI-safe) and requires `CL_DEVICE_SVM_FINE_GRAIN_BUFFER`.
- When available, `tl2::init()` (under `-DTM_ACCEL_GPU`) places the
  guard table in `clSVMAlloc` memory **with the
  `CL_MEM_SVM_FINE_GRAIN_BUFFER` allocation flag** — without it the AMD
  ICD returns coarse-grain memory whose CPU-side mapping is effectively
  uncached (~160 ns/guard access vs ~2 ns; measured as a 30–80×
  backend-wide slowdown). CPU code uses the table through the same
  virtual pointer via `std::atomic`; GPU kernels read the **live**
  table — no staging copies in either direction.
- Kernel-side legality: gfx1151 reports `SVM Atomics: No`; the kernel
  only performs a plain global atomic on the conflict flag (its own
  buffer, read by the CPU after `clFinish`) and plain loads of guards,
  which does not require the SVM-atomics capability.

## Measured performance (Radeon 8060S iGPU, synthetic read-set workload, 2026-11-25)

Read-set sizes K+64 (incl. hot-zone overlap), 8 threads, contended hot
write zone; throughput in commits/s; `bench` = per-entry read-then-write
transactions (source kept off-tree):

| read entries | TL2 | ACCELTL2 (GPU off) | ACCELTL2 (GPU forced) |
|---|---|---|---|
| 128 | 326k | 266k | 64k |
| 1088 | 129k | 136k | 6.2k |
| 16448 | 14.6k | 5.1k* | 4.9k* |
| 65600 | 3.8k | 2.0k* | 1.8k* |

\* default threshold 4096 already dispatches at these sizes.

**Conclusion: on this machine ACCELTL2 is not faster than TL2 in any
measured regime.** Why the pre-abort filter loses despite a per-entry
faster device scan:
1. GPU dispatch+sync floor (≈17 µs) exceeds the whole CPU validation
   scan up to ≥10⁵ entries (CPU scan at 65k entries ≈ 30 µs total
   *including* the rest of the attempt).
2. Every successful commit pays GPU scan **plus** the full CPU
   validation (the clean verdict is not trusted — see safety argument).
3. One shared command queue serializes concurrent dispatches
   (observed: abort rate collapses 89%→3% at K=64 with GPU on — the
   queue acts as a global commit lock).
4. Even with a trusted clean verdict, the read-set *logging* (CPU,
   unavoidable in TL2-style STM) dominates large-read attempts,
   shrinking the winnable fraction.

Keep the backend for: detection/reporting infrastructure, the safe
pre-abort machinery, and future hardware where (a) GPU scan round-trip
is ≪ µs or (b) whole-commit offload is viable. Default `TM_ACCEL_MIN`
(4096) is set so real small/medium transactions never dispatch; on
fine-grain-SVM machines below the threshold the guard table costs ≈ TL2
within noise (1088-entry row).

## Commit-path integration (safety argument)

`commit()` dispatches the pre-scan **before** sorting + lock
acquisition; a device-reported conflict aborts immediately (saving lock
churn on doomed large commits). A *locked-or-changed* guard observed by
the device fails step 5 in the same state, or is about to (the only
divergence is a transient lock whose writer aborts — a spurious abort,
never a missed one). A clean device verdict is **not** trusted: steps
3–5 run in full and remain the correctness authority. Semantics are
therefore identical to TL2 with or without an accelerator; only the
abort *timing* (and spurious-abort rate) can differ, both of which TM
callers must tolerate by design.

## NPU policy (detect, don't use)

AMD XDNA (`/dev/accel/accel0`, `amdxdna`) and similar NPUs are detected
and reported at init, but never dispatched to: they execute
pre-compiled tile graphs over DMA buffers — no arbitrary-pointer
dereference and no coherent atomics over shared host memory, which is
exactly the requirement here (evidence: XDNA/XRT programming model vs
fine-grain SVM support probed per device in `accel_probe.cpp`).

## Limitations / follow-ups

- Single shared command queue: concurrent commits serialize their
  dispatches (fine for pre-abort filtering; revisit if offload becomes
  the hot path).
- The clean-verdict fast path (skip CPU step-5 scan after a device
  "clean" + clock-unchanged proof) is **not** implemented — the naive
  proof is unsound against transient writer locks; needs a guard-state
  epoch. Tracked in TODO.md.
- `TM_ACCEL_DISABLE=1` forces the CPU path.
