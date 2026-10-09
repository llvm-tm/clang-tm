# `tsx_sgl` backend status

**Status:** `production`

TSX + SGL hybrid backend with standard correctness coverage (hardware required for TSX path).

- 2026-10-05: first fuzz on real TSX hardware (intel14v2 Broadwell-EP) found a
  store-buffer race in the SGL entry (`sgl_owner=1` plain store not drained
  before touching data → lost updates, 20/20 fuzz FAIL). Fixed with an
  entry-side `_mm_mfence()`; see `docs/CORRECTNESS_FIXES.md` §19. Post-fix:
  fuzz 4t×10k 10/10 + 2t×50k 5/5 PASS, bank 10/10 PASS, mid-TX-abort
  injection (`TM_TSX_INJECT_PCT`) 13/13 PASS. gem5's Ruby HTM model does not
  expose this class of race (no store-buffer visibility window).
- Diagnostics: `TM_TSX_STATS=1` prints per-run commit/abort/SGL mix at exit.
