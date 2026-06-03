//! TM Address-Space Region Allocator
//!
//! A single-contiguous-64 GB mmap region provides the TM address space.
//! The region is divided into 1 MB slabs, each subdivided into 64 KB
//! chunks.  Each chunk serves one size class, carrying a header at its
//! start.  Small blocks (≤256 B) use a bitmap; medium blocks
//! (257–4096 B) use an offset-based intrusive free list.  Large
//! allocations (>4 KB) are bump-allocated from the slab with a
//! `LargeHdr` prefix for free-path recycling.
//!
//! Per-thread free-list caches avoid atomic RMW on the fast path —
//! `tm_region_free` is a single `memcpy` + pointer swap.
//!
//! Allocator metadata (chunk headers, bitmaps) is inside the mmap
//! region, so `is_tm_address()` is a simple bounds check.

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::OnceLock;

// ════════════════════════════════════════════════════════════════
// Constants
// ════════════════════════════════════════════════════════════════

const TM_REGION_SIZE: usize = 64 * 1024 * 1024 * 1024; // 64 GB
const TM_SLAB_PAGES: usize = 256;
const CHUNK_SIZE: usize = 65536; // 64 KB
const CHUNK_MASK: usize = CHUNK_SIZE - 1;
const CHUNK_HEADER_SZ: usize = 32;
const MAX_CLASSES: usize = 32;
const BITMAP_THRESHOLD: usize = 256;
const TL_FL_WATERMARK: u32 = 256;
const CHUNK_MAGIC: u32 = 0x544D4348; // "TMCH"
const LARGE_MAGIC: u32 = 0x544D4C52; // "TMLR"

// ════════════════════════════════════════════════════════════════
// Data structures (must match C++ layout for ABI compatibility)
// ════════════════════════════════════════════════════════════════

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ChunkHeader {
    pub magic: u32,         // CHUNK_MAGIC
    pub size_class: u16,    // 0..MAX_CLASSES-1
    pub flags: u16,         // bit 0 = bitmap mode
    pub block_size: u32,    // bytes per block
    pub block_count: u32,   // total blocks in this chunk
    pub free_count: u32,    // free blocks remaining
    pub alloc_hint: u32,    // bitmap: next scan index; freelist: head offset
    _pad: [u32; 2],         // padding to 32 bytes
}
const _: () = assert!(std::mem::size_of::<ChunkHeader>() == 32);

#[repr(C)]
#[derive(Copy, Clone)]
pub struct LargeHdr {
    pub magic: u32,       // LARGE_MAGIC
    pub size: u32,        // user-requested size
    pub next: *mut LargeHdr,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct Slab {
    bump: *mut u8,
    end: *mut u8,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct TLFreeList {
    head: *mut u8,
    count: u32,
}

#[repr(C)]
struct TlState {
    slab: Slab,
    free_lists: [TLFreeList; MAX_CLASSES],
    hot_chunks: [*mut ChunkHeader; MAX_CLASSES],
    large_free_list: *mut LargeHdr,
}

impl TlState {
    const fn new() -> Self {
        const FL_INIT: TLFreeList = TLFreeList { head: std::ptr::null_mut(), count: 0 };
        const NULL: *mut ChunkHeader = std::ptr::null_mut();
        TlState {
            slab: Slab { bump: std::ptr::null_mut(), end: std::ptr::null_mut() },
            free_lists: [FL_INIT; MAX_CLASSES],
            hot_chunks: [NULL; MAX_CLASSES],
            large_free_list: std::ptr::null_mut(),
        }
    }
}

thread_local! {
    static TL: UnsafeCell<TlState> = const { UnsafeCell::new(TlState::new()) };
}

#[inline]
fn tl_mut() -> &'static mut TlState {
    unsafe { &mut *TL.with(|c| c.get()) }
}

// ════════════════════════════════════════════════════════════════
// Global state
// ════════════════════════════════════════════════════════════════

static REGION_START: AtomicUsize = AtomicUsize::new(0);
static REGION_END: AtomicUsize = AtomicUsize::new(0);
static SLAB_SIZE: AtomicUsize = AtomicUsize::new(0);
static SLAB_SHIFT: AtomicUsize = AtomicUsize::new(0);
static SLAB_COUNT: AtomicUsize = AtomicUsize::new(0);
static NEXT_SLAB_IDX: AtomicUsize = AtomicUsize::new(0);

// Precomputed size-class tables (set once during init, read-only after)
static SC_BLOCK_SIZE: OnceLock<[u16; MAX_CLASSES]> = OnceLock::new();
static SC_BLOCK_COUNT: OnceLock<[u16; MAX_CLASSES]> = OnceLock::new();
static SC_BITMAP_BYTES: OnceLock<[u16; MAX_CLASSES]> = OnceLock::new();
static SC_DATA_OFF: OnceLock<[u16; MAX_CLASSES]> = OnceLock::new();

// Size-class table at compile time
const SIZE_TABLE: [u16; MAX_CLASSES] = [
    16, 24, 32, 40, 48, 56, 64, 80,
    96, 112, 128, 160, 192, 224, 256, 320,
    384, 448, 512, 640, 768, 896, 1024, 1280,
    1536, 1792, 2048, 2560, 3072, 3584, 4096, 0,
];

// ════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════

/// Returns `true` if `ptr` lies within the TM address-space region.
#[inline]
pub fn is_tm_address(ptr: *const u8) -> bool {
    let start = REGION_START.load(Ordering::Relaxed);
    let end = REGION_END.load(Ordering::Relaxed);
    let addr = ptr as usize;
    addr >= start && addr < end
}

/// Allocate from the TM region.
///
/// Returns a 16-byte-aligned pointer.  `malloc(0)` returns a unique
/// non-null pointer (minimum 16 bytes).
pub fn tm_region_malloc(sz: usize) -> *mut u8 {
    let mut sz = sz;
    if sz < 16 {
        sz = 16;
    }
    sz = (sz + 15) & !15;

    let sc = size_class_for(sz);
    if sc >= MAX_CLASSES || sc_block_size(sc) == 0 {
        return malloc_large(sz);
    }

    // 1. Try per-thread free list
    let tl = tl_mut();
    if let Some(ptr) = tl_fl_pop(&mut tl.free_lists[sc]) {
        return ptr;
    }

    // 2. Try hot chunk
    let mut chunk = tl.hot_chunks[sc];
    if chunk.is_null() {
        chunk = chunk_alloc(sc);
        tl.hot_chunks[sc] = chunk;
    }
    let mut hdr = chunk;

    if unsafe { (*hdr).free_count == 0 } {
        chunk = chunk_alloc(sc);
        tl.hot_chunks[sc] = chunk;
        hdr = chunk;
    }

    // 3. Allocate a block from the chunk
    unsafe {
        if sc_bitmap_bytes(sc) > 0 {
            // Bitmap mode
            let mut bm = chunk_bitmap(hdr);
            let mut idx = bitmap_find_free(bm, (*hdr).block_count as i32, (*hdr).alloc_hint as i32);
            if idx < 0 {
                chunk = chunk_alloc(sc);
                tl.hot_chunks[sc] = chunk;
                hdr = chunk;
                bm = chunk_bitmap(hdr);
                idx = 0;
            }
            bitmap_set(bm, idx as usize);
            (*hdr).alloc_hint = (idx as u32) + 1;
            (*hdr).free_count -= 1;
            chunk_block_ptr(chunk as *mut u8, idx, sc)
        } else {
            // Freelist mode
            let mut off = (*hdr).alloc_hint;
            if off == 0 {
                chunk = chunk_alloc(sc);
                tl.hot_chunks[sc] = chunk;
                hdr = chunk;
                off = (*hdr).alloc_hint;
            }
            (*hdr).alloc_hint = fl_next(chunk as *mut u8, off);
            (*hdr).free_count -= 1;
            (chunk as *mut u8).add(off as usize)
        }
    }
}

pub fn tm_region_calloc(nmemb: usize, sz: usize) -> *mut u8 {
    let total = nmemb * sz;
    let p = tm_region_malloc(total);
    if !p.is_null() {
        unsafe { std::ptr::write_bytes(p, 0, total) };
    }
    p
}

/// Reallocate a pointer obtained from `tm_region_malloc`.
pub fn tm_region_realloc(ptr: *mut u8, new_sz: usize) -> *mut u8 {
    if ptr.is_null() {
        return tm_region_malloc(new_sz);
    }
    if new_sz == 0 {
        tm_region_free(ptr);
        return std::ptr::null_mut();
    }

    let old_sz = old_size(ptr);
    let np = tm_region_malloc(new_sz);
    if !np.is_null() {
        let copy = if old_sz < new_sz { old_sz } else { new_sz };
        unsafe { std::ptr::copy_nonoverlapping(ptr, np, copy) };
    }
    tm_region_free(ptr);
    np
}

/// Free a pointer obtained from `tm_region_malloc`.
///
/// Pushes to the per-thread free list — no atomic RMW.
pub fn tm_region_free(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    if !is_tm_address(ptr) {
        return;
    }

    unsafe {
        let chunk = chunk_start(ptr);
        let hdr = &*(chunk as *const ChunkHeader);

        if hdr.magic == CHUNK_MAGIC
            && (hdr.size_class as usize) < MAX_CLASSES
            && sc_block_size(hdr.size_class as usize) > 0
        {
            let sc = hdr.size_class as usize;
            let tl = tl_mut();
            tl_fl_push(&mut tl.free_lists[sc], ptr);

            // Watermark: if TL list exceeds limit, drain half back to chunks
            if tl.free_lists[sc].count > TL_FL_WATERMARK {
                tl_fl_drain(&mut tl.free_lists[sc], sc);
            }
        } else {
            // Large allocation
            let lh = (ptr as *mut u8).sub(std::mem::size_of::<LargeHdr>()) as *mut LargeHdr;
            if (*lh).magic == LARGE_MAGIC {
                let tl = tl_mut();
                (*lh).next = tl.large_free_list;
                tl.large_free_list = lh;
            }
        }
    }
}

/// Initialise the TM region (must be called before any other function).
///
/// Returns 0 on success, -1 on failure.  Safe to call multiple times
/// (idempotent).
pub fn tm_region_init() -> i32 {
    if REGION_START.load(Ordering::Acquire) != 0 {
        return 0;
    }
    if NEXT_SLAB_IDX
        .compare_exchange(0, 1, Ordering::AcqRel, Ordering::Relaxed)
        .is_err()
    {
        // Another thread is initialising — spin until REGION_START is set
        while REGION_START.load(Ordering::Acquire) == 0 {
            std::hint::spin_loop();
        }
        return 0;
    }

    let page_size = page_size();
    let slab_size = TM_SLAB_PAGES * page_size;
    let slab_shift = slab_size.trailing_zeros() as usize;

    // Precompute size-class tables
    let mut bs: [u16; MAX_CLASSES] = [0; MAX_CLASSES];
    let mut bc: [u16; MAX_CLASSES] = [0; MAX_CLASSES];
    let mut bm: [u16; MAX_CLASSES] = [0; MAX_CLASSES];
    let mut doff: [u16; MAX_CLASSES] = [0; MAX_CLASSES];

    for sc in 0..MAX_CLASSES {
        let bbs = SIZE_TABLE[sc];
        if bbs == 0 {
            break;
        }
        bs[sc] = bbs;

        if (bbs as usize) <= BITMAP_THRESHOLD {
            // Bitmap mode: compute max blocks fitting with bitmap overhead
            let max_try = (CHUNK_SIZE - CHUNK_HEADER_SZ) / (bbs as usize);
            let mut found = 0u32;
            for cnt in (1..=max_try).rev() {
                let bytes = (cnt + 7) >> 3;
                let hdr_total = CHUNK_HEADER_SZ + bytes;
                let data_off = (hdr_total + 15) & !15;
                let total = data_off + cnt * (bbs as usize);
                if total <= CHUNK_SIZE {
                    bm[sc] = bytes as u16;
                    doff[sc] = data_off as u16;
                    found = cnt as u32;
                    break;
                }
            }
            bc[sc] = found as u16;
        } else {
            // Freelist mode: no bitmap
            let cnt = (CHUNK_SIZE - CHUNK_HEADER_SZ) / (bbs as usize);
            bc[sc] = cnt as u16;
            bm[sc] = 0;
            doff[sc] = CHUNK_HEADER_SZ as u16;
        }
    }

    // mmap the region
    let region_size = if std::mem::size_of::<usize>() <= 4 {
        512 * 1024 * 1024 // 512 MB for 32-bit
    } else {
        TM_REGION_SIZE
    };

    let addr = mmap_region(region_size);
    if addr.is_null() {
        NEXT_SLAB_IDX.store(0, Ordering::Release);
        return -1;
    }

    // Align to CHUNK_SIZE
    let aligned_start = ((addr as usize) + CHUNK_MASK) & !CHUNK_MASK;
    let aligned_size = region_size & !CHUNK_MASK;

    let num_slabs = aligned_size >> slab_shift;

    // Store precomputed tables BEFORE releasing REGION_START so readers
    // that see a non-zero REGION_START (Acquire) also see valid tables.
    SC_BLOCK_SIZE.set(bs).ok();
    SC_BLOCK_COUNT.set(bc).ok();
    SC_BITMAP_BYTES.set(bm).ok();
    SC_DATA_OFF.set(doff).ok();

    REGION_START.store(aligned_start, Ordering::Release);
    REGION_END.store(aligned_start + aligned_size, Ordering::Release);
    SLAB_SIZE.store(slab_size, Ordering::Relaxed);
    SLAB_SHIFT.store(slab_shift, Ordering::Relaxed);
    SLAB_COUNT.store(num_slabs, Ordering::Relaxed);
    NEXT_SLAB_IDX.store(0, Ordering::Relaxed);

    eprintln!(
        "[TM-REGION] mmap {:p} .. {:p}  ({} MB, {} slabs, {} B/slab, {} classes)",
        aligned_start as *const u8,
        (aligned_start + aligned_size) as *const u8,
        aligned_size / (1024 * 1024),
        num_slabs,
        slab_size,
        MAX_CLASSES,
    );

    0
}

/// Cleanup (currently a no-op; the OS reclaims the mapping at exit).
pub fn tm_region_destroy() {
    // The OS reclaims the virtual mapping at process exit.
    // We deliberately do NOT reset state to avoid dangling hot-chunk
    // pointers from earlier allocations.
}

/// Return region statistics for monitoring/debugging.
pub struct RegionStats {
    pub slab_size: usize,
    pub slab_count: usize,
    pub next_slab_idx: usize,
}

pub fn tm_region_stats() -> Option<RegionStats> {
    if REGION_START.load(Ordering::Acquire) == 0 {
        return None;
    }
    Some(RegionStats {
        slab_size: SLAB_SIZE.load(Ordering::Relaxed),
        slab_count: SLAB_COUNT.load(Ordering::Relaxed),
        next_slab_idx: NEXT_SLAB_IDX.load(Ordering::Relaxed),
    })
}

// ════════════════════════════════════════════════════════════════
// Internal helpers
// ════════════════════════════════════════════════════════════════

fn page_size() -> usize {
    // Safe: the POSIX `sysconf` call is reentrant and does not retain
    // pointers.  Fallback to 4096 if the value is bogus.
    let ps = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    if ps <= 0 { 4096 } else { ps as usize }
}

fn mmap_region(size: usize) -> *mut u8 {
    let addr = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            size,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
            -1,
            0,
        )
    };
    if addr == libc::MAP_FAILED {
        std::ptr::null_mut()
    } else {
        addr as *mut u8
    }
}

// ── Table accessors ─────────────────────────────────────────

#[inline]
fn sc_block_size(sc: usize) -> u16 {
    SC_BLOCK_SIZE.get().unwrap()[sc]
}

#[inline]
fn sc_block_count(sc: usize) -> u16 {
    SC_BLOCK_COUNT.get().unwrap()[sc]
}

#[inline]
fn sc_bitmap_bytes(sc: usize) -> u16 {
    SC_BITMAP_BYTES.get().unwrap()[sc]
}

#[inline]
fn sc_data_off(sc: usize) -> u16 {
    SC_DATA_OFF.get().unwrap()[sc]
}

// ── Size-class lookup ───────────────────────────────────────

#[inline]
fn size_class_for(sz: usize) -> usize {
    for i in 0..MAX_CLASSES {
        let bs = sc_block_size(i);
        if bs == 0 {
            break;
        }
        if (bs as usize) >= sz {
            return i;
        }
    }
    MAX_CLASSES
}

// ── Chunk address helpers ──────────────────────────────────

#[inline]
fn chunk_start(ptr: *mut u8) -> *mut u8 {
    ((ptr as usize) & !CHUNK_MASK) as *mut u8
}

#[inline]
fn chunk_hdr<'a>(ptr: *mut u8) -> &'a ChunkHeader {
    unsafe { &*(chunk_start(ptr) as *const ChunkHeader) }
}

#[inline]
fn chunk_block_ptr(chunk: *mut u8, block_idx: i32, sc: usize) -> *mut u8 {
    let off = (sc_data_off(sc) as usize) + (block_idx as usize) * (sc_block_size(sc) as usize);
    unsafe { chunk.add(off) }
}

#[inline]
fn chunk_bitmap<'a>(hdr: *mut ChunkHeader) -> &'a mut [u64] {
    unsafe {
        let ptr = (hdr as *mut u8).add(CHUNK_HEADER_SZ) as *mut u64;
        let words = ((*hdr).block_count as usize + 63) >> 6;
        std::slice::from_raw_parts_mut(ptr, words.max(1))
    }
}

// ── Bitmap helpers ──────────────────────────────────────────

fn bitmap_find_free(bm: &[u64], block_count: i32, hint: i32) -> i32 {
    let words = (block_count as usize + 63) >> 6;
    let start_w = (hint as usize) >> 6;
    for pass in 0..2 {
        let range = if pass == 0 {
            start_w..words
        } else {
            0..start_w
        };
        for i in range {
            let w = unsafe { std::ptr::read_volatile(&bm[i]) };
            if w != !0u64 {
                let bit = (!w).trailing_zeros();
                let idx = ((i << 6) | (bit as usize)) as i32;
                if idx < block_count {
                    return idx;
                }
            }
        }
    }
    -1
}

fn bitmap_set(bm: &mut [u64], idx: usize) {
    unsafe {
        let word = &mut bm[idx >> 6] as *mut u64;
        let mask = 1u64 << (idx & 63);
        std::ptr::write_volatile(word, std::ptr::read_volatile(word) | mask);
    }
}

fn bitmap_clear(bm: &mut [u64], idx: usize) {
    unsafe {
        let word = &mut bm[idx >> 6] as *mut u64;
        let mask = 1u64 << (idx & 63);
        std::ptr::write_volatile(word, std::ptr::read_volatile(word) & !mask);
    }
}

// ── Freelist helpers (offset-based) ─────────────────────────

#[inline]
fn fl_next(chunk: *mut u8, off: u32) -> u32 {
    if off == 0 {
        return 0;
    }
    unsafe {
        let mut nxt: u32 = 0;
        std::ptr::copy_nonoverlapping(chunk.add(off as usize), &mut nxt as *mut u32 as *mut u8, 4);
        nxt
    }
}

#[inline]
unsafe fn fl_set_next(chunk: *mut u8, off: u32, nxt: u32) {
    std::ptr::copy_nonoverlapping(&nxt as *const u32 as *const u8, chunk.add(off as usize), 4);
}

// ── Thread-local free-list helpers ─────────────────────────

#[inline]
fn tl_fl_push(fl: &mut TLFreeList, ptr: *mut u8) {
    unsafe {
        std::ptr::copy_nonoverlapping(&fl.head as *const *mut u8 as *const u8, ptr, 8);
    }
    fl.head = ptr;
    fl.count = fl.count.wrapping_add(1);
}

#[inline]
fn tl_fl_pop(fl: &mut TLFreeList) -> Option<*mut u8> {
    let ptr = fl.head;
    if ptr.is_null() {
        return None;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(ptr, &mut fl.head as *mut *mut u8 as *mut u8, 8);
    }
    fl.count = fl.count.wrapping_sub(1);
    Some(ptr)
}

/// Drain half of a TL free list back to its chunks.
///
/// When `fl.count` exceeds `TL_FL_WATERMARK`, this pops `count/2` entries,
/// finds each entry's chunk, and returns the block to the chunk (clears the
/// bitmap bit for small classes or pushes the offset onto the chunk's
/// freelist for medium classes).
fn tl_fl_drain(fl: &mut TLFreeList, sc: usize) {
    let to_drain = fl.count / 2;
    let b_size = sc_block_size(sc) as usize;
    let d_off = sc_data_off(sc) as usize;
    let is_bitmap = b_size <= BITMAP_THRESHOLD;

    for _ in 0..to_drain {
        let ptr = match tl_fl_pop(fl) {
            Some(p) => p,
            None => break,
        };
        unsafe {
            let chunk = chunk_start(ptr);
            let hdr = &mut *(chunk as *mut ChunkHeader);
            if is_bitmap {
                let idx = (ptr as usize - chunk as usize - d_off) / b_size;
                let bm = chunk_bitmap(chunk as *mut ChunkHeader);
                bitmap_clear(bm, idx);
            } else {
                let off = (ptr as usize - chunk as usize) as u32;
                fl_set_next(chunk, off, hdr.alloc_hint);
                hdr.alloc_hint = off;
            }
            hdr.free_count += 1;
        }
    }
}

// ── Large allocation ────────────────────────────────────────

fn malloc_large(sz: usize) -> *mut u8 {
    let tl = tl_mut();

    // 1. Check per-thread large free list
    if !tl.large_free_list.is_null() {
        let h = lh_mut(tl.large_free_list);
        if (h.size as usize) >= sz {
            tl.large_free_list = h.next;
            h.size = sz as u32;
            return unsafe { (h as *mut LargeHdr as *mut u8).add(std::mem::size_of::<LargeHdr>()) };
        }
    }

    // 2. Bump-allocate from slab
    let alloc_sz = (sz + std::mem::size_of::<LargeHdr>() + 15) & !15;

    let p = tl.slab.bump;
    let next = unsafe { p.add(alloc_sz) };
    if next > tl.slab.end {
        // Claim a fresh slab
        let slab_sz = SLAB_SIZE.load(Ordering::Relaxed);
        let needed = (alloc_sz + slab_sz - 1) >> SLAB_SHIFT.load(Ordering::Relaxed);
        let idx = NEXT_SLAB_IDX.fetch_add(needed, Ordering::Relaxed);
        let slab = unsafe {
            (REGION_START.load(Ordering::Relaxed) as *mut u8)
                .add(idx * slab_sz)
        };
        if unsafe { slab.add(needed * slab_sz) } as usize > REGION_END.load(Ordering::Relaxed) {
            eprintln!("FATAL: TM region exhausted");
            std::process::abort();
        }
        tl.slab.bump = unsafe { slab.add(alloc_sz) };
        tl.slab.end = unsafe { slab.add(needed * slab_sz) };
        let lh = slab as *mut LargeHdr;
        lh_ptr(lh).magic = LARGE_MAGIC;
        lh_ptr(lh).size = sz as u32;
        lh_ptr(lh).next = std::ptr::null_mut();
        return unsafe { slab.add(std::mem::size_of::<LargeHdr>()) };
    } else {
        tl.slab.bump = next;
    }

    let lh = p as *mut LargeHdr;
    lh_ptr(lh).magic = LARGE_MAGIC;
    lh_ptr(lh).size = sz as u32;
    lh_ptr(lh).next = std::ptr::null_mut();
    unsafe { p.add(std::mem::size_of::<LargeHdr>()) }
}

// ── Chunk allocator (slow path) ─────────────────────────────

fn chunk_alloc(sc: usize) -> *mut ChunkHeader {
    let tl = tl_mut();

    // Bump-allocate a CHUNK_SIZE block from the current slab
    let p = tl.slab.bump;
    let next = unsafe { p.add(CHUNK_SIZE) };
    if next > tl.slab.end {
        let slab_sz = SLAB_SIZE.load(Ordering::Relaxed);
        let idx = NEXT_SLAB_IDX.fetch_add(1, Ordering::Relaxed);
        let slab = unsafe {
            (REGION_START.load(Ordering::Relaxed) as *mut u8)
                .add(idx * slab_sz)
        };
        if unsafe { slab.add(slab_sz) } as usize > REGION_END.load(Ordering::Relaxed) {
            eprintln!("FATAL: TM region exhausted");
            std::process::abort();
        }
        tl.slab.bump = unsafe { slab.add(CHUNK_SIZE) };
        tl.slab.end = unsafe { slab.add(slab_sz) };
        init_chunk(slab as *mut ChunkHeader, sc);
        return slab as *mut ChunkHeader;
    } else {
        tl.slab.bump = next;
    }

    init_chunk(p as *mut ChunkHeader, sc);
    p as *mut ChunkHeader
}

fn init_chunk(hdr: *mut ChunkHeader, sc: usize) {
    unsafe {
        let bs = sc_block_size(sc);
        let bcnt = sc_block_count(sc);
        let d_off = sc_data_off(sc);
        let bm_bytes = sc_bitmap_bytes(sc);

        (*hdr).magic = CHUNK_MAGIC;
        (*hdr).size_class = sc as u16;
        (*hdr).flags = 0;
        (*hdr).block_size = bs as u32;
        (*hdr).block_count = bcnt as u32;
        (*hdr).free_count = bcnt as u32;

        if bm_bytes > 0 {
            // Bitmap mode: zero bitmap (all blocks free)
            (*hdr).flags = 1;
            (*hdr).alloc_hint = 0;
            std::ptr::write_bytes(
                (hdr as *mut u8).add(CHUNK_HEADER_SZ),
                0,
                bm_bytes as usize,
            );
        } else {
            // Freelist mode: link all blocks
            for i in 0..(bcnt as u32 - 1) {
                let cur = (d_off as u32) + i * (bs as u32);
                let nxt = (d_off as u32) + (i + 1) * (bs as u32);
                fl_set_next(hdr as *mut u8, cur, nxt);
            }
            // Last block terminates the list
            let last_off = (d_off as u32) + (bcnt as u32 - 1) * (bs as u32);
            fl_set_next(hdr as *mut u8, last_off, 0);

            (*hdr).alloc_hint = d_off as u32;
        }
    }
}

// ── Size discovery for realloc ──────────────────────────────

fn old_size(ptr: *mut u8) -> usize {
    unsafe {
        let chunk = chunk_start(ptr);
        let hdr = &*(chunk as *const ChunkHeader);
        if hdr.magic == CHUNK_MAGIC && (hdr.size_class as usize) < MAX_CLASSES - 1 {
            sc_block_size(hdr.size_class as usize) as usize
        } else {
            let lh = &*((ptr as *mut u8).sub(std::mem::size_of::<LargeHdr>()) as *mut LargeHdr);
            if lh.magic == LARGE_MAGIC {
                lh.size as usize
            } else {
                0
            }
        }
    }
}



// Tiny helpers to work around `*mut LargeHdr` field access in `static mut`
fn lh_mut(ptr: *mut LargeHdr) -> &'static mut LargeHdr {
    unsafe { &mut *ptr }
}
fn lh_ptr(ptr: *mut LargeHdr) -> &'static mut LargeHdr {
    unsafe { &mut *ptr }
}

// ════════════════════════════════════════════════════════════════
// Tests
// ════════════════════════════════════════════════════════════════

#[cfg(test)]
mod tests {
    use super::*;

    fn init_once() {
        assert_eq!(tm_region_init(), 0);
    }

    // ── test_init ──────────────────────────────────────────────
    #[test]
    fn test_init() {
        init_once();
        assert!(REGION_START.load(Ordering::Relaxed) > 0);
    }

    // ── test_malloc_small ──────────────────────────────────────
    #[test]
    fn test_malloc_small() {
        init_once();
        for sz in [1, 4, 8, 16, 32, 64, 128, 256] {
            let p = tm_region_malloc(sz);
            assert!(!p.is_null(), "malloc({}) returned null", sz);
            assert_eq!(p as usize & 15, 0, "malloc({}) unaligned", sz);
        }
    }

    // ── test_malloc_large ──────────────────────────────────────
    #[test]
    fn test_malloc_large() {
        init_once();
        for sz in [1024, 4096, 16384, 65536, 262144, 1048576] {
            let p = tm_region_malloc(sz);
            assert!(!p.is_null(), "malloc({}) returned null", sz);
            assert_eq!(p as usize & 15, 0, "malloc({}) unaligned", sz);
            // Write and read back (page-fault the memory)
            unsafe { std::ptr::write_bytes(p, 0xAB, sz / 4) };
            let val = unsafe { std::ptr::read(p) };
            assert_eq!(val, 0xAB);
        }
    }

    // ── test_is_tm_address ─────────────────────────────────────
    #[test]
    fn test_is_tm_address() {
        init_once();
        let p = tm_region_malloc(64);
        assert!(is_tm_address(p));
        assert!(!is_tm_address(std::ptr::null()));
        assert!(!is_tm_address(Box::into_raw(Box::new(0u8))));
    }

    // ── test_calloc ────────────────────────────────────────────
    #[test]
    fn test_calloc() {
        init_once();
        let p = tm_region_calloc(4, 64);
        assert!(!p.is_null());
        for i in 0..256 {
            assert_eq!(unsafe { std::ptr::read(p.add(i)) }, 0);
        }
    }

    // ── test_realloc ───────────────────────────────────────────
    #[test]
    fn test_realloc() {
        init_once();
        // realloc(null, sz) == malloc(sz)
        let p = tm_region_realloc(std::ptr::null_mut(), 64);
        assert!(!p.is_null());

        // realloc(p, 0) == free(p)
        let q = tm_region_realloc(p, 0);
        assert!(q.is_null());

        // realloc(p, larger)
        let r = tm_region_malloc(16);
        unsafe { std::ptr::write_bytes(r, 0x42, 16) };
        let s = tm_region_realloc(r, 128);
        assert!(!s.is_null());
        if s != r {
            // Data was copied
            for i in 0..16 {
                assert_eq!(unsafe { std::ptr::read(s.add(i)) }, 0x42);
            }
        }
    }

    // ── test_zero_size ────────────────────────────────────────
    #[test]
    fn test_zero_size() {
        init_once();
        let a = tm_region_malloc(0);
        let b = tm_region_malloc(0);
        assert!(!a.is_null());
        assert!(!b.is_null());
        assert_ne!(a, b, "malloc(0) must return distinct pointers");
    }

    // ── test_bump_order ───────────────────────────────────────
    #[test]
    fn test_bump_order() {
        init_once();
        const N: usize = 256;
        let mut ptrs: [*mut u8; N] = [std::ptr::null_mut(); N];
        for i in 0..N {
            ptrs[i] = tm_region_malloc(16);
            assert!(!ptrs[i].is_null());
        }
        // All pointers should be 16-byte aligned and non-overlapping
        // (same-chunk order should be ascending)
        for i in 0..N {
            for j in i + 1..N {
                assert_ne!(ptrs[i], ptrs[j]);
            }
        }
    }

    // ── test_oversized ─────────────────────────────────────────
    #[test]
    fn test_oversized() {
        init_once();
        let slab_sz = SLAB_SIZE.load(Ordering::Relaxed);
        let sz = slab_sz * 2 + 1;
        let p = tm_region_malloc(sz);
        assert!(!p.is_null());
        assert!(is_tm_address(p));
        unsafe { std::ptr::write_bytes(p, 0xFF, sz / 4) };
    }

    // ── test_free_reuse ────────────────────────────────────────
    #[test]
    fn test_free_reuse() {
        init_once();
        const N: usize = 1000;
        let mut ptrs: [*mut u8; N] = [std::ptr::null_mut(); N];
        for i in 0..N {
            ptrs[i] = tm_region_malloc(32);
            unsafe { std::ptr::write(ptrs[i] as *mut u32, i as u32) };
        }
        // Free all
        for i in 0..N {
            tm_region_free(ptrs[i]);
        }
        // Re-allocate — blocks should be recycled from TL list
        for i in 0..N {
            ptrs[i] = tm_region_malloc(32);
            assert!(!ptrs[i].is_null());
            unsafe { std::ptr::write(ptrs[i] as *mut u32, (i + 1000) as u32) };
        }
        // Verify data
        for i in 0..N {
            let val = unsafe { std::ptr::read(ptrs[i] as *mut u32) };
            assert_eq!(val, (i + 1000) as u32);
        }
        // Clean up
        for i in 0..N {
            tm_region_free(ptrs[i]);
        }
    }

    // ── test_multi_thread ───────────────────────────────────────
    #[test]
    fn test_multi_thread() {
        init_once();
        const ALLOCS_PER: usize = 1000;
        let nthreads = std::thread::available_parallelism()
            .map(|v| v.get())
            .unwrap_or(4);
        let mut handles = Vec::new();

        for t in 0..nthreads {
            handles.push(std::thread::spawn(move || {
                let mut local: Vec<usize> = Vec::with_capacity(ALLOCS_PER);
                for i in 0..ALLOCS_PER {
                    let sz = ((i % 8) + 1) * 16;
                    let p = tm_region_malloc(sz);
                    assert!(!p.is_null(), "thread {}: null at iter {}", t, i);
                    unsafe { std::ptr::write(p as *mut u32, (t * 64 + i) as u32) };
                    local.push(p as usize);
                }
                local
            }));
        }

        // Collect and verify
        for (t, h) in handles.into_iter().enumerate() {
            let ptrs = h.join().expect("thread panicked");
            for (i, &addr) in ptrs.iter().enumerate() {
                let p = addr as *mut u8;
                let val = unsafe { std::ptr::read(p as *mut u32) };
                assert_eq!(val, (t * 64 + i) as u32,
                           "thread {} iter {}: data mismatch", t, i);
            }
            // Clean up this thread's allocations
            for &addr in &ptrs {
                tm_region_free(addr as *mut u8);
            }
        }
    }
}
