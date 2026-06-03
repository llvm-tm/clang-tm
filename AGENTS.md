# Session Summary

## Latest Session (this session — 2026-06-03)

### Rust TM address-space region allocator (`addrspace` crate)
- **New `rust_tm_api/addrspace/` crate**: 64 GB mmap'd TM region, 32 size classes (16 B–4 KB), small-block bitmaps (≤256 B), medium-block offset-based free lists, large bump allocation (>4 KB).
- **Per-thread free-list cache**: `TLFreeList { head, count }` with `tl_fl_push`/`tl_fl_pop` (single `memcpy` + pointer swap, no atomic RMW). `tm_region_free` actually recycles memory (TL push, not a no-op).
- **TL-list watermark draining**: When per-class free list exceeds 256 entries, drains half back to chunk bitmaps or freelists so blocks become available to other threads.
- **Fixes during development**:
  - `static [u16; MAX_CLASSES]` → `OnceLock<[u16; MAX_CLASSES]>` (Rust puts statics in read-only memory; writing via `*mut` cast causes SIGSEGV).
  - CAS-loser race: thread that fails `compare_exchange(0, 1)` for `NEXT_SLAB_IDX` previously returned 0 before tables populated → now spin-waits for `REGION_START != 0`.
  - Size-class tables stored before `REGION_START.store(Release)` ensures Acquire-load readers see valid tables.
- **11 unit tests PASS** (init, malloc_small, malloc_large, alignment, is_tm_address, calloc, realloc, zero_size, bump_order, oversized, free_reuse, multi_thread).
- **Linked-list stress benchmark** (8 phases, mirrors C++ `test_region_stress.cpp`):
  - Single-threaded: **31.3M allocs/sec** (TM) vs 16.1M (std::alloc) → **1.9×**
  - Multi-threaded (4t): **62.5M allocs/sec** (TM) vs 15.2M (std::alloc) → **4.1×**
  - Scalability: 1t→17.9M, 2t→35.7M, 4t→55.6M (near-linear)
  - Phase 7 (alloc/free > 2 GB total): 134M iterations in 2.5s, region not exhausted (recycling works).
- **Integration with `tm-executor`**: `spec_alloc`/`spec_free` re-exports, `ensure_region_init()` in `QueueExecutor::new()` and worker threads (idempotent via `OnceLock`). 6 tests pass.
- **`tm-executor/Cargo.toml`**: Added full backend feature passthrough (wbctl/wbetl/wt/norec/tl2/swisstm/dudetm/tsxsgl/nvhtm/spht) matching `benchmarks` crate pattern.
- **C++ allocator micro-optimizations**: Division → bit-shift for power-of-2 aligned sizes (`/ g_slab_size` → `>> g_slab_size_shift`, `% 64` → `& 63`, `/ 64` → `>> 6`).
- **Commit `3fd3e02`**: Rust addrspace crate + tm-executor integration + C++ micro-optimizations.

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
