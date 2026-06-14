use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering, fence};
use std::sync::atomic::AtomicU32;
use std::sync::OnceLock;

#[allow(unused_imports)]
pub use runtime_core::{tm_install_tmx_hook, Primitive, TypedValue, WriteBack};
#[cfg(any(feature = "wbctl", feature = "wbetl"))]
pub use runtime_core::TmxAbort;

// ── Constants ───────────────────────────────────────────
const LOCK_MASK: u64 = 0xFF;
const VERSION_SHIFT: u64 = 8;
pub const TABLE_BITS: u64 = 20;
const TABLE_SIZE: usize = 1 << TABLE_BITS;

pub fn lock_index(addr: usize) -> usize {
    let h = (addr as u64).wrapping_mul(0x9E3779B97F4A7C15);
    (h >> (64 - TABLE_BITS)) as usize
}

// ── Lock table ──────────────────────────────────────────
struct Lock {
    data: AtomicU64,
}

impl Lock {
    const fn new() -> Self {
        Lock { data: AtomicU64::new(0) }
    }

    pub fn is_locked(&self) -> bool {
        self.data.load(Ordering::Relaxed) & LOCK_MASK != 0
    }

    pub fn try_lock_exclusive(&self) -> bool {
        let cur = self.data.load(Ordering::Relaxed);
        if cur & LOCK_MASK != 0 {
            return false;
        }
        self.data
            .compare_exchange_weak(cur, cur | 1, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }

    pub fn unlock_exclusive(&self) {
        let cur = self.data.load(Ordering::Relaxed);
        let ver = (cur & !LOCK_MASK) >> VERSION_SHIFT;
        self.data.store((ver + 1) << VERSION_SHIFT, Ordering::Release);
    }

    pub fn version(&self) -> u64 {
        let v = self.data.load(Ordering::Acquire);
        (v & !LOCK_MASK) >> VERSION_SHIFT
    }
}

// ── Public lock helpers ─────────────────────────────────
pub fn is_locked(addr: usize) -> bool { locks()[lock_index(addr)].is_locked() }

pub fn read_version(addr: usize) -> u64 { locks()[lock_index(addr)].version() }

pub fn try_lock_at_index(idx: usize) -> bool { locks()[idx].try_lock_exclusive() }

pub fn unlock_at_index(idx: usize) { locks()[idx].unlock_exclusive(); }

pub fn version_at_index(idx: usize) -> u64 { locks()[idx].version() }

#[cfg(feature = "wbctl")]
fn lock_at_index(idx: usize) {
    while !try_lock_at_index(idx) {
        std::hint::spin_loop();
    }
}

static LOCK_TABLE: OnceLock<Box<[Lock]>> = OnceLock::new();

fn locks() -> &'static [Lock] {
    LOCK_TABLE.get_or_init(|| {
        (0..TABLE_SIZE).map(|_| Lock::new()).collect::<Vec<_>>().into_boxed_slice()
    })
}

// ── Global clock ────────────────────────────────────────
static G_CLOCK: AtomicU64 = AtomicU64::new(0);

pub fn gc_snapshot() -> u64 { G_CLOCK.load(Ordering::Acquire) }

pub fn gc_acquire() {
    loop {
        let cur = G_CLOCK.load(Ordering::Relaxed);
        if cur & 1 == 0
            && G_CLOCK.compare_exchange_weak(cur, cur | 1, Ordering::Acquire, Ordering::Relaxed).is_ok()
        { return; }
        std::hint::spin_loop();
    }
}

pub fn gc_release_and_inc() { G_CLOCK.fetch_add(1, Ordering::Release); }

// ── Write entry ─────────────────────────────────────────
pub struct WriteEntry {
    pub value: TypedValue,
}

// ── Transaction state ───────────────────────────────────
pub struct TxState {
    pub read_set: Vec<(usize, u64)>,
    pub write_set: HashMap<usize, WriteEntry>,
    /// Deferred write-back closures (safe to apply at commit).
    /// Used by write-back backends (WBCTL, WBETL).
    #[cfg(any(feature = "wbctl", feature = "wbetl"))]
    pub write_backs: Vec<WriteBack>,
    /// Deferred undo closures (safe to apply on abort/rollback).
    /// Used by write-through backends (WT).
    #[cfg(feature = "wt")]
    pub undo_backs: Vec<WriteBack>,
    pub start_version: u64,
    #[allow(dead_code)]
    pub aborted: bool,
    /// Lock indices held by encounter-time locking variants (WBETL, WT).
    #[allow(dead_code)]
    pub locked_addrs: Vec<usize>,
}

impl TxState {
    pub fn new(start_version: u64) -> Self {
        TxState {
            read_set: Vec::with_capacity(64),
            write_set: HashMap::with_capacity(8),
            #[cfg(any(feature = "wbctl", feature = "wbetl"))]
            write_backs: Vec::new(),
            #[cfg(feature = "wt")]
            undo_backs: Vec::new(),
            start_version,
            aborted: false,
            locked_addrs: Vec::new(),
        }
    }
}

// ── Thread-local state ──────────────────────────────────
thread_local! {
    pub(crate) static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}

pub fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    TX.with(|tx| {
        let mut b = tx.borrow_mut();
        f(b.as_mut().expect("no active transaction"))
    })
}

/// Global abort counter, incremented on every TM abort across all backends.
pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);

pub fn tm_abort_count() -> u64 {
    TM_ABORT_COUNT.load(Ordering::Relaxed)
}

// ── TM_STATS counters ─────────────────────────────────────
pub static TM_COMMIT_COUNT: AtomicU64 = AtomicU64::new(0);
pub static TM_TOTAL_READS: AtomicU64 = AtomicU64::new(0);
pub static TM_TOTAL_WRITES: AtomicU64 = AtomicU64::new(0);
pub static TM_MAX_READ_SET: AtomicU64 = AtomicU64::new(0);
pub static TM_MAX_WRITE_SET: AtomicU64 = AtomicU64::new(0);
pub static TM_MIN_READ_SET: AtomicU64 = AtomicU64::new(u64::MAX);
pub static TM_MIN_WRITE_SET: AtomicU64 = AtomicU64::new(u64::MAX);

pub fn update_read_write_stats(read_len: usize, write_len: usize) {
    let r = read_len as u64;
    let w = write_len as u64;
    TM_TOTAL_READS.fetch_add(r, Ordering::Relaxed);
    TM_TOTAL_WRITES.fetch_add(w, Ordering::Relaxed);
    TM_COMMIT_COUNT.fetch_add(1, Ordering::Relaxed);
    // max
    let mut cur = TM_MAX_READ_SET.load(Ordering::Relaxed);
    while r > cur { match TM_MAX_READ_SET.compare_exchange_weak(cur, r, Ordering::Relaxed, Ordering::Relaxed) { Ok(_) => break, Err(v) => cur = v, } }
    cur = TM_MAX_WRITE_SET.load(Ordering::Relaxed);
    while w > cur { match TM_MAX_WRITE_SET.compare_exchange_weak(cur, w, Ordering::Relaxed, Ordering::Relaxed) { Ok(_) => break, Err(v) => cur = v, } }
    // min
    let mut cur_min = TM_MIN_READ_SET.load(Ordering::Relaxed);
    while r < cur_min { match TM_MIN_READ_SET.compare_exchange_weak(cur_min, r, Ordering::Relaxed, Ordering::Relaxed) { Ok(_) => break, Err(v) => cur_min = v, } }
    cur_min = TM_MIN_WRITE_SET.load(Ordering::Relaxed);
    while w < cur_min { match TM_MIN_WRITE_SET.compare_exchange_weak(cur_min, w, Ordering::Relaxed, Ordering::Relaxed) { Ok(_) => break, Err(v) => cur_min = v, } }
}

pub fn reset_tm_stats() {
    TM_COMMIT_COUNT.store(0, Ordering::Relaxed);
    TM_TOTAL_READS.store(0, Ordering::Relaxed);
    TM_TOTAL_WRITES.store(0, Ordering::Relaxed);
    TM_MAX_READ_SET.store(0, Ordering::Relaxed);
    TM_MAX_WRITE_SET.store(0, Ordering::Relaxed);
    TM_MIN_READ_SET.store(u64::MAX, Ordering::Relaxed);
    TM_MIN_WRITE_SET.store(u64::MAX, Ordering::Relaxed);
    TM_ABORT_COUNT.store(0, Ordering::Relaxed);
}

pub fn tx_active() -> bool {
    TX.with(|tx| tx.borrow().is_some())
}

pub fn flush_tx() -> Option<Box<TxState>> { TX.with(|tx| tx.borrow_mut().take()) }

// ── Commit helpers ──────────────────────────────────────

/// Lock lock-indices for a set of write-addresses.
#[cfg(feature = "wbctl")]
pub fn lock_write_addrs(addrs: &[usize]) -> Vec<usize> {
    let mut idxs: Vec<usize> = addrs.iter().map(|&a| lock_index(a)).collect();
    idxs.sort_unstable();
    idxs.dedup();
    for &idx in &idxs { lock_at_index(idx); }
    fence(Ordering::SeqCst);
    idxs
}

pub fn validate_read_set(read_set: &[(usize, u64)]) -> bool {
    read_set.iter().all(|(a, v)| version_at_index(lock_index(*a)) == *v)
}

pub fn unlock_indices(idxs: &[usize]) {
    // Dedup: lock aliasing (two addresses → same lock index) causes
    // duplicates in WT/WBETL locked_addrs.  Each lock must be released
    // exactly once or the version counter is inflated.
    let mut deduped: Vec<usize> = idxs.to_vec();
    deduped.sort_unstable();
    deduped.dedup();
    for &idx in &deduped { unlock_at_index(idx); }
    fence(Ordering::SeqCst);
}

// ── Init ────────────────────────────────────────────────
static INIT_COUNT: AtomicU32 = AtomicU32::new(0);

fn do_init() {
    if INIT_COUNT.fetch_add(1, Ordering::Relaxed) == 0 {
        addrspace::tm_region_init();
        locks();
        G_CLOCK.store(0, Ordering::Release);
    }
}

fn do_exit() {
    let cc = TM_COMMIT_COUNT.load(Ordering::Relaxed);
    let tr = TM_TOTAL_READS.load(Ordering::Relaxed);
    let tw = TM_TOTAL_WRITES.load(Ordering::Relaxed);
    let mxr = TM_MAX_READ_SET.load(Ordering::Relaxed);
    let mxw = TM_MAX_WRITE_SET.load(Ordering::Relaxed);
    let mnr = TM_MIN_READ_SET.load(Ordering::Relaxed);
    let mnw = TM_MIN_WRITE_SET.load(Ordering::Relaxed);
    let ac = TM_ABORT_COUNT.load(Ordering::Relaxed);
    print!("TM_STATS: commits={}", cc);
    if cc > 0 {
        print!(" avg_reads={:.1} min_reads={} max_reads={} avg_writes={:.1} min_writes={} max_writes={}",
            tr as f64 / cc as f64, mnr, mxr, tw as f64 / cc as f64, mnw, mxw);
    }
    println!(" aborts={}", ac);
    INIT_COUNT.store(0, Ordering::Relaxed);
}

// ── Public API (shared by all variants) ─────────────────
pub fn tm_init() { tm_install_tmx_hook(); do_init(); }
pub fn tm_exit() { do_exit(); }
pub fn tm_init_thread() {}
pub fn tm_exit_thread() {}

pub fn tm_begin() {
    let start_ver = gc_snapshot();
    TX.with(|tx| { *tx.borrow_mut() = Some(Box::new(TxState::new(start_ver))); });
}

// ── Macros for public API wrappers ──────────────────────
#[macro_export]
macro_rules! def_read  { ($n:ident, $t:ty) => { #[inline] pub fn $n(addr: *mut $t) -> $t { read_word::<$t>(addr as usize) } }; }
#[macro_export]
macro_rules! def_write { ($n:ident, $t:ty) => { #[inline] pub fn $n(addr: *mut $t, val: $t) { write_word::<$t>(addr as usize, val); } }; }
