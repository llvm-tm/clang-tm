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
  guard table in `clSVMAlloc` memory. CPU code uses it through the same
  virtual pointer via `std::atomic`; GPU kernels read the **live**
  table — no staging copies in either direction.
- Measured on Radeon 8060S: kernel dispatch+sync floor ≈ 17 µs; CPU
  scans at ~2–4 ns/entry → crossover ~5–10k entries, hence the default
  `TM_ACCEL_MIN=4096`.

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
