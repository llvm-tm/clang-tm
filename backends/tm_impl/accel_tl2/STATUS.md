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
