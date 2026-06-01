use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering, fence};
use std::sync::atomic::AtomicU32;
use std::sync::OnceLock;

pub use runtime_core::{Primitive, TypedValue};

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
    pub start_version: u64,
    pub aborted: bool,
    /// Lock indices held by encounter-time locking variants (WBETL, WT).
    pub locked_addrs: Vec<usize>,
    /// Undo log for write-through variants (WT) — restored on abort.
    pub undo_log: Vec<(usize, TypedValue)>,
}

impl TxState {
    pub fn new(start_version: u64) -> Self {
        TxState {
            read_set: Vec::with_capacity(64),
            write_set: HashMap::with_capacity(8),
            start_version,
            aborted: false,
            locked_addrs: Vec::new(),
            undo_log: Vec::new(),
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

pub fn tx_active() -> bool {
    TX.with(|tx| match *tx.borrow() { Some(ref t) => !t.aborted, None => false })
}

pub fn flush_tx() -> Option<Box<TxState>> { TX.with(|tx| tx.borrow_mut().take()) }

// ── Memory operations ───────────────────────────────────
#[inline]
pub unsafe fn write_mem(addr: usize, val: u64, nbytes: u8) {
    let ptr = addr as *mut u64;
    match nbytes {
        1 => (ptr as *mut u8).write(val as u8),
        2 => (ptr as *mut u16).write(val as u16),
        4 => (ptr as *mut u32).write(val as u32),
        8 => ptr.write(val),
        _ => panic!("write_mem: unsupported size {nbytes}"),
    }
}

#[inline]
pub unsafe fn write_mem_bytes(addr: usize, buf: &[u8]) {
    let dst = addr as *mut u8;
    for (i, &b) in buf.iter().enumerate() {
        dst.add(i).write(b);
    }
}

// ── Commit helpers ──────────────────────────────────────
pub type RawWriteSet = Vec<(usize, u64, u8)>;
pub type RawByteWriteSet = Vec<(usize, Box<[u8]>)>;

/// Flatten the typed write-set into primitive and byte halves.
pub fn flatten_write_set(write_set: &HashMap<usize, WriteEntry>) -> (RawWriteSet, RawByteWriteSet) {
    let mut raw = Vec::new();
    let mut raw_bytes = Vec::new();
    for (&addr, entry) in write_set {
        match entry.value {
            TypedValue::Bytes(ref b) => raw_bytes.push((addr, b.clone())),
            _ => raw.push((addr, entry.value.as_u64(), entry.value.byte_size() as u8)),
        }
    }
    (raw, raw_bytes)
}

/// Lock lock-indices for both primitive and byte write sets.
pub fn lock_write_addrs_both(raw: &RawWriteSet, raw_bytes: &RawByteWriteSet) -> Vec<usize> {
    let mut idxs: Vec<usize> = raw.iter().map(|(a, _, _)| lock_index(*a)).collect();
    idxs.extend(raw_bytes.iter().map(|(a, _)| lock_index(*a)));
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
    for &idx in idxs { unlock_at_index(idx); }
    fence(Ordering::SeqCst);
}

// ── Init ────────────────────────────────────────────────
static INIT_COUNT: AtomicU32 = AtomicU32::new(0);

fn do_init() {
    if INIT_COUNT.fetch_add(1, Ordering::Relaxed) == 0 {
        locks();
        G_CLOCK.store(0, Ordering::Release);
    }
}

fn do_exit() { INIT_COUNT.store(0, Ordering::Relaxed); }

// ── Public API (shared by all variants) ─────────────────
pub fn tm_init() { do_init(); }
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
