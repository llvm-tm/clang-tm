# `accel_tl2` (ACCELTL2) backend status

**Status:** `incubating`

TL2 core + runtime-detected accelerator validation (zero-copy fine-grain
SVM guard table + GPU read-set prevalidation). Validated on an AMD
Ryzen AI MAX+ 395 (Radeon 8060S iGPU, `gfx1151`, OpenCL/ROCm): test_tx
120/120 and test_ds 207/207 pass both at the default dispatch threshold
(4096 read entries — accelerator idles for small logs) and with
`TM_ACCEL_MIN=1`, which forces every writing commit through the GPU
prevalidation kernel — including an 8-thread contended `bank` run with
prevalidation on: 222,353 GPU dispatches over the shared SVM guard table,
49,936 device-detected conflicts aborted early, money conserved. Degrades
bit-identically to plain TL2 wherever no fine-grain-SVM device exists
(also verified: no OpenCL runtime at all).

See `Implementation_notes.md` and `accel_probe.hpp` for the design and
the measured crossover rationale.

## 2026-11-26 — differential-driver validation and sanitizer status

The bank run above shows the device path *runs*; what it does not show is that
commits validated by the device still produce a correct history. Re-running the
T1 differential driver (`tests/concurrency/drv_diff_ops.cpp`, whose five oracles
are order-independent) with `TM_ACCEL_MIN=1 -T 8 -n 3000`, seeds 1–3: 24.5k
prevalidations dispatched and 98–164 conflicts found by the device per run, with
`v_range`, `v_readown`, `v_ptr`, `quiesce_viol` and `alloc_viol` all 0 and exit
status 0 — the first end-to-end check of the shared-SVM guard table against an
oracle rather than a single-program invariant. `bank` conservation holds in the
same configuration. Measured cost on this iGPU: forcing prevalidation on every
commit costs ~39× throughput (64k vs 2.5M tx/s at `-t 4 -n 20000`), which is why
the default threshold is 4096 entries and why CI forces the threshold rather
than trusting the default to exercise the path.

ThreadSanitizer on this backend reports 3 issues (2 data races, 1 thread leak)
that are **not** backend code: the racing threads are created inside
`libhsa-runtime64.so.1` (the ROCm runtime behind the OpenCL ICD), the addresses
lie in the driver's own allocations, and all three still appear with `-T 1`,
where the STM layer has no worker threads at all. With `TM_ACCEL_DISABLE=1` the
same binary reports 0. `ci.yml`'s `tsan-gate` therefore gates ACCELTL2's CPU
path, and `nightly.yml`'s `accel-e2e` job covers the device path (correctness
always, plus an assertion that a present fine-grain-SVM device really was used).

