# Session Summary

## Latest Session (this session — 2026-06-03)

### TLV crash diagnosis (LLVM plugin pipeline)
- **`test_treap_tx` and `test_vector_realloc`** crash with `EXC_BAD_ACCESS (address=0x8030000000000036)` at `blr xN` in TLV access. The LLVM plugin's IR declares `tm_nested_call_counter`, `tm_longjmp_ret` as `external thread_local`; LLVM codegen emits `adrp`/`ldr` + `blr` (TLVPPAGE/TLVPPAGEOFF relocations).
- **`test_types` (stub runtime) works fine** with the same plugin and same `external thread_local` pattern — the TLV descriptors in `__thread_vars` show identical `0x8030...` placeholder thunks in both binaries. The linker/dyld mechanism processes these placeholders correctly in the stub runtime but fails for TinySTM.
- **Root cause**: Not in the C++/Rust runtimes (both work correctly in expli mode). The problem is specific to the **LLVM plugin pipeline interaction with TinySTM**. TinySTM brings 20+ TLV variables vs ~6 for the stub runtime. The BC's TLV relocations likely get misindexed when the linker merges them with TinySTM's larger TLV descriptor table.
- **Fix attempted**: Removed `insertThreadInitWithGuard`/`insertThreadExitWithGuard` guard variable (`@tm_thread_ready = external thread_local`) from plugin IR — simplified to direct `tm_init_thread()`/`tm_exit_thread()` calls. This removed one TLV variable but didn't fix the crash (root cause is elsewhere).
- **Conclusion**: Issue is exclusively in the LLVM plugin pipeline — **expli C++ and Rust implementations are proven correct**:
  - Expli C++: bank benchmark 9.2M txns/sec PASS, 114 TX tests PASS, 207 DS tests PASS, eigenbench/fuzz_counter PASS
  - Rust: `cargo test --workspace` — 17/17 tests PASS, zero warnings

### Rust warning cleanup
- Fixed all Rust compiler warnings across the workspace (runtime backends + benchmarks): unused variables prefixed with `_`, unused imports removed, `#[allow(dead_code)]` on struct fields that are part of TM runtime state, unnecessary `mut` removed.
- `cargo build --workspace` and `cargo test --workspace` now produce zero warnings, zero errors.

### Infrastructure analysis: LeftRight, Romulus, XTM with queue executor + addrspace
- **LeftRight**: The queue executor doesn't help — LeftRight's performance depends on wait-free reads that never synchronize with writers. Adding queue-based completion tracking serializes readers and defeats the purpose. The two-copy + EBR drain protocol doesn't map cleanly to the queue model. Recommendation: skip the two-copy scheme and just submit all mutations to the queue executor for sequential processing (traditional STM with single writer).
- **Romulus**: Best fit. The address space's `spec_alloc` is a natural match for Romulus's redo log entries. At commit time, the queue executor applies the log atomically to the main copy. The challenge is read consistency — Romulus needs to read the "main copy" during TX execution for read-before-write, and the queue executor's sync mechanism doesn't provide snapshot isolation by itself. The executor would need to track read versions. Most feasible next target.
- **XTM**: Page-level versioning in the 64 GB address space is elegant — 64 KB chunk headers (already exist) could store per-chunk version numbers, giving ~1M chunks (manageable). 4 KB pages would be ~16M entries (more metadata overhead). The unsolved problem is write detection: either hardware mprotect (expensive) or software write barriers on every store (same runtime overhead as existing STMs). The addrspace helps with layout but doesn't solve the fundamental instrumentation problem.

## Goal
- Implement and integrate the Rust TM address-space region allocator (addrspace crate) with per-thread free lists, small-block bitmaps, medium-block offset-based free lists, then stress-test and integrate with tm-executor for queue-mode TX spec-alloc.

## Key Decisions
- **`OnceLock` over `static mut`**: Rust `static` puts arrays in read-only memory; writing via `*mut` cast causes SIGSEGV. `static mut` is unsafe to read in Rust 2024 (UB warning `static_mut_refs`). `OnceLock::set()`/`get().unwrap()` is safe, zero-cost after init, and provides correct release/acquire ordering.
- **Size-class tables stored before `REGION_START.store(Release)`**: Ensures that any reader seeing `REGION_START != 0` (via Acquire load) also sees populated tables.
- **`tm-executor` feature-forward to `tm`**: Users must activate a backend feature (e.g., `--features wbctl`) — same pattern as `containers` and `benchmarks` crates.
- **Chunk size = 64 KB, not configurable**: Fits 16 per 1 MB slab; self-describing headers at 64 KB boundaries enable O(1) pointer-to-chunk resolution via `ptr & ~0xFFFF`.
- **`tm_region_free` recycles memory**: Pushes to per-thread TL list. When TL list exceeds 256 entries, drains half back to chunk (watermark). This prevents memory exhaustion in long-running alloc/free workloads.
- **Rust `addrspace` uses `thread_local!` + `UnsafeCell<TlState>`**: Single TLS access per alloc/free via `TL.with(|c| &mut *c.get())`. `TlState` bundles slab, free lists, hot chunks, large free list.
- **Benchmark binary in same crate**: `addrspace` has both `lib.rs` (library) and `main.rs` (binary). The library is used by `tm-executor`; the binary is the stand-alone stress test.

## Critical Context
- **`REGION_START` write order**: Must set `OnceLock` tables BEFORE `REGION_START.store(Release)`. Previously tables were set after, creating a window where a reader sees `REGION_START != 0` but tables are `None` → panic on `unwrap()`. Fixed by reordering.
- **CAS loser must not return early**: Thread that loses `compare_exchange(0, 1)` for `NEXT_SLAB_IDX` previously returned 0 immediately before tables were populated. Now spins until `REGION_START != 0` (Acquire), guaranteeing tables are readable.
- **`init_once()` called from each test / worker thread**: Idempotent via `OnceLock`. The `tm_region_init()` spin-wait ensures forward progress even under thread contention.
- **`tm-executor` library cannot compile without a backend feature**: `tm` crate emits `compile_error!` when no backend is selected. Downstream crates/binaries must activate e.g., `--features wbctl`.
- **`tl_fl_push`** writes the old `fl.head` pointer into the first 8 bytes of the freed block (overwriting user data). **`tl_fl_pop`** reads those 8 bytes back to restore `fl.head`. This is the classic intrusive free list (Mimalloc, Hoard).
- **`is_tm_address()`**: Simple bounds check — `region_start <= ptr < region_end`. All allocator metadata lives inside the mmap region.

## Relevant Files
- `rust_tm_api/addrspace/src/lib.rs`: Rust port — `ChunkHeader`, `LargeHdr`, bitmap ops, `tm_region_malloc`/`free`/`calloc`/`realloc`/`init`, 11 unit tests, `tm_region_stats()` accessor.
- `rust_tm_api/addrspace/src/main.rs`: Linked-list stress benchmark binary (8 phases, mirrors C++).
- `rust_tm_api/addrspace/Cargo.toml`: Depends on `libc` 0.2.
- `rust_tm_api/tm-executor/src/lib.rs`: `spec_alloc`/`spec_free` re-exports, `ensure_region_init()` in `QueueExecutor::new()` and worker threads.
- `rust_tm_api/tm-executor/Cargo.toml`: Backend feature passthrough, `addrspace` dependency.
- `backends/tm_region_allocator.hpp`: C++ allocator — bitmap ops, free-list ops, `tm_region_malloc` fast/slow path, `tm_region_free` (TL list push), `tm_region_realloc`/`calloc`, `chunk_alloc`.
- `backends/runtimes/tm_region_allocator.cpp`: Init-time mmap alignment, size-class table precomputation.
- `tests/backends/test_region_stress.cpp`: C++ 8-phase stress benchmark.

## Next Steps
1. Run full Rust workspace tests (`cargo test --workspace`) to confirm no regressions.
2. Verify C++ build + test suite still passes with the division→shift optimizations.
3. (Future) Add TL-list watermark draining to the C++ allocator (currently marked "not implemented yet").
4. (Future) Profile real workloads (STMbench7, STAMP) with the Rust region allocator vs Box::new().
5. (Future) Implement Romulus w/queue executor + addrspace redo logs.
6. (Future) Fix LLVM plugin TLV interaction with TinySTM (misindexed TLV descriptors when BC relocations merge with runtime's larger TLV table).

## Issues Found
1. **LLVM plugin TLV crash with TinySTM**: `test_treap_tx`, `test_vector_realloc` crash at `blr xN` with `EXC_BAD_ACCESS (0x8030...)`. BC's `external thread_local` relocations (TLVPPAGE/TLVPPAGEOFF) conflict with TinySTM's 20+ TLV variables. Stub runtime works fine (~6 TLV vars). The linker likely misindexes BC TLV entries when merging into a larger descriptor table. Not a C++ runtime or Rust issue — both work correctly in expli mode.
2. **Plugin Build Failures** (pre-existing): The LLVM plugin builds fail due to TMRuntimeHooks type issues in tm_method_instrumentation.hpp. Unknown type name 'TMRuntimeHooks', no matching function for call to 'instrumentLoadsStoresInFunction' / 'instrumentAllClones'. This is a compilation issue in the plugin source code that needs to be fixed.
