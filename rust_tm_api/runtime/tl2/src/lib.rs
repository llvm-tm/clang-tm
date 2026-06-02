// ── TL2 TM backend for Rust ────────────────────────────
// Commit-time locking with a global commit lock.
// Uses a shared lock table (hash-based, 2^20 entries) and
// a global monotonically increasing clock.

use core::sync::atomic::{fence, AtomicU64, Ordering};
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::OnceLock;
pub use runtime_core::{Primitive, TypedValue, WriteBack};

// ── Constants ───────────────────────────────────────────
const LOCK_MASK: u64 = 0xFF;
const VERSION_SHIFT: u64 = 8;
const TABLE_BITS: u64 = 20;
const TABLE_SIZE: usize = 1 << TABLE_BITS;

fn lock_index(addr: usize) -> usize {
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
    fn is_locked(&self) -> bool {
        self.data.load(Ordering::Relaxed) & LOCK_MASK != 0
    }
    fn try_lock_exclusive(&self) -> bool {
        let cur = self.data.load(Ordering::Relaxed);
        if cur & LOCK_MASK != 0 { return false; }
        self.data.compare_exchange_weak(cur, cur | 1, Ordering::Acquire, Ordering::Relaxed).is_ok()
    }
    fn unlock_exclusive(&self) {
        let cur = self.data.load(Ordering::Relaxed);
        let ver = (cur & !LOCK_MASK) >> VERSION_SHIFT;
        self.data.store((ver + 1) << VERSION_SHIFT, Ordering::Release);
    }
    fn version(&self) -> u64 {
        let v = self.data.load(Ordering::Acquire);
        (v & !LOCK_MASK) >> VERSION_SHIFT
    }
}

fn lock_at_index(idx: usize) -> &'static Lock {
    &locks()[idx]
}

fn try_lock(addr: usize) -> bool {
    lock_at_index(lock_index(addr)).try_lock_exclusive()
}

fn unlock(addr: usize) {
    lock_at_index(lock_index(addr)).unlock_exclusive();
}

fn read_version(addr: usize) -> u64 {
    lock_at_index(lock_index(addr)).version()
}

fn is_locked(addr: usize) -> bool {
    lock_at_index(lock_index(addr)).is_locked()
}

static LOCK_TABLE: OnceLock<Box<[Lock]>> = OnceLock::new();

fn locks() -> &'static [Lock] {
    LOCK_TABLE.get_or_init(|| {
        (0..TABLE_SIZE).map(|_| Lock::new()).collect::<Vec<_>>().into_boxed_slice()
    })
}

// ── Global commit lock + clock ──────────────────────────
// Commit lock: 0 = unlocked, >0 = locked by thread.
static COMMIT_LOCK: AtomicU64 = AtomicU64::new(0);
static G_CLOCK: AtomicU64 = AtomicU64::new(0);

pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);

// ── Transaction state ───────────────────────────────────
struct TxState {
    read_set: Vec<(usize, u64)>,  // (addr, version_at_read)
    write_set: HashMap<usize, TypedValue>,
    /// Deferred write-back closures (safe to apply at commit).
    write_backs: Vec<WriteBack>,
    start_version: u64,
    aborted: bool,
}

impl TxState {
    fn new(start_version: u64) -> Self {
        TxState {
            read_set: Vec::with_capacity(64),
            write_set: HashMap::with_capacity(8),
            write_backs: Vec::new(),
            start_version,
            aborted: false,
        }
    }
}

// ── Thread-local state ──────────────────────────────────
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
    TX.with(|tx| match *tx.borrow() { Some(ref t) => !t.aborted, None => false })
}

fn tx_aborted() -> bool {
    TX.with(|tx| match *tx.borrow() { Some(ref t) => t.aborted, None => false })
}

fn flush_tx() -> Option<Box<TxState>> {
    TX.with(|tx| tx.borrow_mut().take())
}

// ── Read word ───────────────────────────────────────────
fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }

    // Check write-set
    if let Some(tv) = TX.with(|tx| {
        tx.borrow().as_ref().and_then(|t| t.write_set.get(&addr).cloned())
    }) { return T::from_typed(&tv); }

    loop {
        // Spin while lock held
        while is_locked(addr) { std::hint::spin_loop(); }
        let ver = read_version(addr);
        let val: T = unsafe { (addr as *const T).read() };
        // Double-check the lock version after reading
        if read_version(addr) != ver { continue; }

        let aborted = with_tx(|tx| {
            if ver > tx.start_version {
                tx.aborted = true;
                true
            } else {
                // Check read_set size cap
                if tx.read_set.len() > 1_000_000 {
                    tx.aborted = true;
                    return true;
                }
                tx.read_set.push((addr, ver));
                false
            }
        });
        if aborted { return val; }
        return val;
    }
}

// ── Write word ──────────────────────────────────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if tx_aborted() { return; }
    if !tx_active() { unsafe { (addr as *mut T).write(val); } return; }

    let tv = val.to_typed();
    with_tx(|tx| {
        tx.write_set.insert(addr, tv.clone());
        tx.write_backs.push(tv.into_write_back(addr));
    });
}

// ── Raw byte operations ─────────────────────────────────
fn read_raw_bytes(addr: usize, dst: &mut [u8]) {
    for (i, byte) in dst.iter_mut().enumerate() { *byte = read_word::<u8>(addr + i); }
}

fn write_raw_bytes(addr: usize, src: &[u8]) {
    fence(Ordering::SeqCst);
    if tx_aborted() { return; }
    if !tx_active() { unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len()); } return; }
    let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
    with_tx(|tx| {
        tx.write_set.insert(addr, tv.clone());
        tx.write_backs.push(tv.into_write_back(addr));
    });
}

// ── Validate read-set (lock-based) ─────────────────────
fn validate_read_set(rs: &[(usize, u64)]) -> bool {
    rs.iter().all(|(a, v)| read_version(*a) == *v)
}

// ── Commit ──────────────────────────────────────────────
pub fn tm_commit() -> bool {
    let tx = match flush_tx() { Some(t) => t, None => return true };
    fence(Ordering::SeqCst);

    if tx.aborted { TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed); return false; }
    if tx.write_set.is_empty() { return true; }

    // 1. Acquire global commit lock
    let my_id = 1; // single-bit thread ID for the global lock
    loop {
        let cur = COMMIT_LOCK.load(Ordering::Relaxed);
        if cur == 0 && COMMIT_LOCK.compare_exchange_weak(0, my_id, Ordering::Acquire, Ordering::Relaxed).is_ok() {
            break;
        }
        std::hint::spin_loop();
    }

    // 2. Snapshot the clock
    let commit_version = G_CLOCK.load(Ordering::Acquire);

    // 3. Validate read-set
    if !validate_read_set(&tx.read_set) {
        COMMIT_LOCK.store(0, Ordering::Release);
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        return false;
    }

    // 4. Lock all write-set addresses (sorted for deadlock freedom)
    let mut addrs: Vec<usize> = tx.write_set.keys().copied().collect();
    addrs.sort_unstable();
    let mut locked_idxs: Vec<usize> = Vec::with_capacity(addrs.len());
    for &a in &addrs {
        let idx = lock_index(a);
        // Dedup lock indices (two addresses may map to same lock)
        if locked_idxs.last().copied() != Some(idx) {
            while !lock_at_index(idx).try_lock_exclusive() {
                std::hint::spin_loop();
            }
            locked_idxs.push(idx);
        }
    }
    fence(Ordering::SeqCst);

    // 5. Write-back all values (safe — WriteBack::apply() encapsulates the unsafe)
    for wb in tx.write_backs {
        wb.apply();
    }

    // 6. Unlock write-set addresses (reverse order to match locking)
    for &idx in locked_idxs.iter().rev() {
        lock_at_index(idx).unlock_exclusive();
    }
    fence(Ordering::SeqCst);

    // 7. Advance global clock
    G_CLOCK.fetch_add(1, Ordering::Release);

    // 8. Release global commit lock
    COMMIT_LOCK.store(0, Ordering::Release);

    true
}

// ── Init ────────────────────────────────────────────────
pub fn tm_init() {
    locks();
    G_CLOCK.store(0, Ordering::Release);
}

pub fn tm_exit() {}

pub fn tm_init_thread() {}

pub fn tm_exit_thread() {}

pub fn tm_begin() {
    let sv = G_CLOCK.load(Ordering::Acquire);
    TX.with(|tx| { *tx.borrow_mut() = Some(Box::new(TxState::new(sv))); });
}

pub fn tm_abort_count() -> u64 { TM_ABORT_COUNT.load(Ordering::Relaxed) }

// ── Typed wrappers ─────────────────────────────────────
macro_rules! def_read {
    ($n:ident, $t:ty) => { #[inline] pub fn $n(addr: *mut $t) -> $t { read_word::<$t>(addr as usize) } };
}
macro_rules! def_write {
    ($n:ident, $t:ty) => { #[inline] pub fn $n(addr: *mut $t, val: $t) { write_word::<$t>(addr as usize, val) } };
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

#[inline] pub fn tm_read_ptr<T>(addr: *mut *mut T) -> *mut T { read_word::<u64>(addr as usize) as *mut T }
#[inline] pub fn tm_write_ptr<T>(addr: *mut *mut T, val: *mut T) { write_word::<u64>(addr as usize, val as u64); }
#[inline] pub fn tm_read_raw(addr: *mut u8, dst: &mut [u8]) { read_raw_bytes(addr as usize, dst); }
#[inline] pub fn tm_write_raw(addr: *mut u8, src: &[u8]) { write_raw_bytes(addr as usize, src); }
