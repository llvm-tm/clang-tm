// ── SPHT Rust backend ───────────────────────────────────
// Scalable Persistent Hardware Transactions.
//
// Write-back TM with commit-time locking (WBCTL-style).
// Uses a lock table, global clock, write-set + read-set
// for conflict detection, null-address guard on writes.
//
// In the C++ version, SPHT uses Intel RTM for hardware
// transactions and a per-thread commit log (PCL) for
// epoch-based group durability.  The Rust variant uses
// software TM with commit-time locking for correctness
// and stubs the durability path with atomic fences.

use core::sync::atomic::{fence, AtomicU64, Ordering};
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::OnceLock;

pub use runtime_core::{Primitive, TypedValue, WriteBack};

// ── Constants ───────────────────────────────────────────
const TABLE_BITS: u64 = 20;
const TABLE_SIZE: usize = 1 << TABLE_BITS;
const NULL_ADDR_LIMIT: usize = 0x100000;

fn lock_index(addr: usize) -> usize {
    let h = (addr as u64).wrapping_mul(0x9E3779B97F4A7C15);
    (h >> (64 - TABLE_BITS)) as usize
}

fn is_null_addr(addr: usize) -> bool {
    addr < NULL_ADDR_LIMIT || (addr >> 63) != 0
}

// ── Lock table ──────────────────────────────────────────
struct Lock {
    data: AtomicU64,
}

impl Lock {
    const fn new() -> Self {
        Lock { data: AtomicU64::new(0) }
    }

    fn try_lock_exclusive(&self) -> bool {
        let cur = self.data.load(Ordering::Relaxed);
        if cur & 1 != 0 { return false; }
        self.data
            .compare_exchange_weak(cur, cur | 1, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }

    fn unlock_exclusive(&self) {
        let cur = self.data.load(Ordering::Relaxed);
        let next = ((cur >> 1) + 1) << 1;
        self.data.store(next, Ordering::Release);
    }

    fn version(&self) -> u64 {
        self.data.load(Ordering::Acquire) >> 1
    }

    fn is_locked(&self) -> bool {
        self.data.load(Ordering::Relaxed) & 1 != 0
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

fn gc_acquire() {
    loop {
        let cur = G_CLOCK.load(Ordering::Relaxed);
        if cur & 1 == 0
            && G_CLOCK.compare_exchange_weak(cur, cur | 1, Ordering::Acquire, Ordering::Relaxed).is_ok()
        { return; }
        std::hint::spin_loop();
    }
}

fn gc_release_and_inc() { G_CLOCK.fetch_add(1, Ordering::Release); }

// ── Transaction state ───────────────────────────────────
struct TxState {
    write_set: HashMap<usize, TypedValue>,
    /// Deferred write-back closures (safe to apply at commit).
    write_backs: Vec<WriteBack>,
    read_set: Vec<(usize, u64)>,
    snapshot: u64,
    aborted: bool,
}

impl TxState {
    fn new(snapshot: u64) -> Self {
        TxState {
            write_set: HashMap::with_capacity(8),
            write_backs: Vec::new(),
            read_set: Vec::with_capacity(64),
            snapshot,
            aborted: false,
        }
    }
}

thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}

fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    TX.with(|tx| {
        let mut b = tx.borrow_mut();
        f(b.as_mut().expect("no active transaction"))
    })
}

fn tx_active() -> bool {
    TX.with(|tx| match *tx.borrow() {
        Some(ref t) => !t.aborted,
        None => false,
    })
}

fn tx_aborted() -> bool {
    TX.with(|tx| match *tx.borrow() {
        Some(ref t) => t.aborted,
        None => false,
    })
}

fn flush_tx() -> Option<Box<TxState>> {
    TX.with(|tx| tx.borrow_mut().take())
}

fn lock_write_addrs(write_set: &HashMap<usize, TypedValue>) -> Vec<usize> {
    let mut idxs: Vec<usize> = write_set.keys().map(|&a| lock_index(a)).collect();
    idxs.sort_unstable();
    idxs.dedup();
    for &idx in &idxs {
        loop {
            if locks()[idx].try_lock_exclusive() { break; }
            std::hint::spin_loop();
        }
    }
    fence(Ordering::SeqCst);
    idxs
}

fn unlock_indices(idxs: &[usize]) {
    for &idx in idxs {
        locks()[idx].unlock_exclusive();
    }
    fence(Ordering::SeqCst);
}

// ── Read word ───────────────────────────────────────────
fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }

    // Check write-set first
    if let Some(tv) = with_tx(|tx| tx.write_set.get(&addr).cloned()) {
        return T::from_typed(&tv);
    }

    // Read from memory with version check
    loop {
        while locks()[lock_index(addr)].is_locked() {
            std::hint::spin_loop();
        }
        let version = locks()[lock_index(addr)].version();
        let value: T = unsafe { (addr as *const T).read() };
        if locks()[lock_index(addr)].version() != version {
            continue;
        }
        with_tx(|tx| {
            if version > tx.snapshot {
                tx.aborted = true;
            } else {
                tx.read_set.push((addr, version));
            }
        });
        return value;
    }
}

// ── Write word ──────────────────────────────────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if tx_aborted() { return; }
    if !tx_active() {
        unsafe { (addr as *mut T).write(val); }
        return;
    }
    if is_null_addr(addr) { return; }

    let tv = val.to_typed();
    with_tx(|tx| {
        tx.write_set.insert(addr, tv.clone());
        tx.write_backs.push(tv.into_write_back(addr));
    });
}

// ── Raw byte operations ─────────────────────────────────
fn read_raw_bytes(addr: usize, dst: &mut [u8]) {
    for (i, byte) in dst.iter_mut().enumerate() {
        *byte = read_word::<u8>(addr + i);
    }
}

fn write_raw_bytes(addr: usize, src: &[u8]) {
    fence(Ordering::SeqCst);
    if tx_aborted() { return; }
    if !tx_active() {
        unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len()); }
        return;
    }
    if is_null_addr(addr) { return; }
    let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
    with_tx(|tx| {
        tx.write_set.insert(addr, tv.clone());
        tx.write_backs.push(tv.into_write_back(addr));
    });
}

// ── Public API ──────────────────────────────────────────
pub fn tm_init() {
    locks();
    G_CLOCK.store(0, Ordering::Release);
}

pub fn tm_exit() {}

pub fn tm_init_thread() {}
pub fn tm_exit_thread() {}

pub fn tm_begin() {
    let snapshot = loop {
        let v = G_CLOCK.load(Ordering::Acquire);
        if v & 1 == 0 { break v; }
        std::hint::spin_loop();
    };
    TX.with(|tx| {
        *tx.borrow_mut() = Some(Box::new(TxState::new(snapshot)));
    });
}

pub fn tm_commit() -> bool {
    let tx = match flush_tx() {
        Some(t) => t,
        None => return true,
    };
    fence(Ordering::SeqCst);

    if tx.aborted {
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        return false;
    }

    if tx.write_set.is_empty() {
        return true;
    }

    // Phase 1: acquire global commit lock
    gc_acquire();
    fence(Ordering::SeqCst);

    // Phase 2: acquire write locks
    let idxs = lock_write_addrs(&tx.write_set);

    // Phase 3: validate read-set
    for &(addr, version) in &tx.read_set {
        if locks()[lock_index(addr)].version() != version {
            unlock_indices(&idxs);
            gc_release_and_inc();
            TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
            return false;
        }
    }

    // Phase 4: write-back to memory (safe — WriteBack::apply() encapsulates the unsafe)
    for wb in tx.write_backs {
        wb.apply();
    }

    // Durability fence
    fence(Ordering::SeqCst);

    // Phase 5: release locks
    unlock_indices(&idxs);

    // Phase 6: release commit lock and advance clock
    gc_release_and_inc();

    true
}

pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);

pub fn tm_abort_count() -> u64 {
    TM_ABORT_COUNT.load(Ordering::Relaxed)
}

// ── Typed read/write wrappers ──────────────────────────
macro_rules! def_read {
    ($n:ident, $t:ty) => {
        #[inline]
        pub fn $n(addr: *mut $t) -> $t {
            read_word::<$t>(addr as usize)
        }
    };
}
macro_rules! def_write {
    ($n:ident, $t:ty) => {
        #[inline]
        pub fn $n(addr: *mut $t, val: $t) {
            write_word::<$t>(addr as usize, val)
        }
    };
}

def_read!(tm_read_u8, u8);
def_read!(tm_read_u16, u16);
def_read!(tm_read_u32, u32);
def_read!(tm_read_u64, u64);
def_read!(tm_read_i8, i8);
def_read!(tm_read_i16, i16);
def_read!(tm_read_i32, i32);
def_read!(tm_read_i64, i64);
def_read!(tm_read_f32, f32);
def_read!(tm_read_f64, f64);

def_write!(tm_write_u8, u8);
def_write!(tm_write_u16, u16);
def_write!(tm_write_u32, u32);
def_write!(tm_write_u64, u64);
def_write!(tm_write_i8, i8);
def_write!(tm_write_i16, i16);
def_write!(tm_write_i32, i32);
def_write!(tm_write_i64, i64);
def_write!(tm_write_f32, f32);
def_write!(tm_write_f64, f64);

#[inline]
pub fn tm_read_ptr<T>(addr: *mut *mut T) -> *mut T {
    read_word::<u64>(addr as usize) as *mut T
}

#[inline]
pub fn tm_write_ptr<T>(addr: *mut *mut T, val: *mut T) {
    write_word::<u64>(addr as usize, val as u64);
}

#[inline]
pub fn tm_read_raw(addr: *mut u8, dst: &mut [u8]) {
    read_raw_bytes(addr as usize, dst);
}

#[inline]
pub fn tm_write_raw(addr: *mut u8, src: &[u8]) {
    write_raw_bytes(addr as usize, src);
}
