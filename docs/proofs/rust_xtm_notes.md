# Rust XTM — TLA+ Model Notes

## Status

The C++ XTM model (`XTM.tla`) models page-granularity private-copy TM
(XADT/XSW/XF via `mprotect`).  The Rust XTM at
`expli_instr/rust/workspace/runtime/xtm/src/lib.rs` uses version-table OCC
(the same algorithm as the Rust Romulus backend) because Rust's ownership
model cannot express the page-fault-based private-copy scheme.

**Architecturally, Rust XTM ≈ Rust Romulus.** Both are version-table OCC with:
- `VERSION_TABLE[0..2^20-1]` — per-address version entries
- `G_CLOCK` — global monotonic clock
- `COMMIT_LOCK` — serializes write-back
- Read-set validation (capture version → read data → record)
- Write-back then version-table update

## Model mapping

| Feature | C++ XTM (`XTM.tla`) | Rust XTM | Closest model |
|---------|---------------------|----------|---------------|
| Address granularity | Page (`mprotect`) | Word (version table) | `Romulus.tla` |
| Ownership | XADT (page-level CAS) | None (OCC) | `Romulus.tla` |
| Read-set | Version per page | Version per addr | `Romulus.tla` |
| Commit | Validate + write-back + release | Validate + lock + write-back + fence + update | `Romulus.tla` |
| Fences | RMW ordering only | `fence(SeqCst)` at read/write/commit | `Romulus.tla` + extras |

## Recommended approach

Use the existing `Romulus.tla` as the authoritative model for Rust XTM's
algorithm.  The Rust XTM implementation note at the top of `lib.rs` already
documents this:

> This Rust implementation uses a version-table OCC protocol (same as the
> Rust Romulus backend) because Rust's ownership model cannot express the
> C++ XTM's page-granularity private-copy scheme.

No separate `RustXTM.tla` model is needed.  Cross-reference this note from
the XTM audit report.
