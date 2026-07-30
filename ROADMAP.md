# ROADMAP

## Completed

- 18 TLA+ backend models (safety — all pass, liveness — 5 pass)
- 14 PlusCal conversions (SGL, TL2, TinySTM_WBCTL/WBETL/WT, XTM, LEFTRIGHT, SwissTM, Romulus, NOrec, DUDETM, DESEngine, NVHTM, SPHT, TiKV)
- TMTypes naming conflict fix (13 backends + 15 cfg files)
- Simulator cost mode + calibration against real C++ NOrec
- Rust backend optimizations (wbctl 30–60× high-contention perf)
- jmp_buf meta-invariant plan written

## In Progress

- **GPU STM backends** (Tier 5 — new)
  - P0: PR-STM CUDA C++ implementation
  - P2: TLA+/PlusCal model for GPU warp semantics
  - P3: CSMV backend, AccelerateSTM backend, benchmark ports

## Planned (in priority order)

### Tier 1 — Correctness

| Item | Priority | Status |
|------|----------|--------|
| GPU PR-STM PlusCal model (warp lockstep, priority contention, large read-sets) | P0 | **now** |
| PR-STM CUDA C++ backend | P0 | plan written |
| PR-STM TLC model checking | P2 | after PlusCal |
| CSMV Multi-versioned GPU STM | P3 | design phase |
| AccelerateSTM Obstruction-free GPU STM | P5 | design phase |

### Tier 2 — Verification

| Item | Priority | Status |
|------|----------|--------|
| GPU STM TLA+ safety invariants (lock coherence, warp-phase sync) | P2 | not started |
| GPU STM liveness (priority inversion freedom) | P2 | not started |
| Meta-invariant jmp_buf model (Option A) | P3 | plan written |

### Tier 3 — Benchmarks

| Item | Priority | Status |
|------|----------|--------|
| Port bank/fuzz_counter to CUDA PR-STM | P3 | not started |
| GPU STM vs CPU STM throughput comparison | P4 | not started |

## Long-term

- 20+ backend TLA+ model corpus (all 18 current + 2 GPU)
- Cross-platform GPU STM (AMD ROCm, NVIDIA CUDA)
- Hybrid CPU-GPU TM (cooperative transactions across host/device boundary)
