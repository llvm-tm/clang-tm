use std::cell::RefCell;
use std::sync::atomic::{fence, AtomicU64, Ordering};

use runtime_core::{Primitive, TypedValue, WriteBack};

// ── Globals ──────────────────────────────────────────────────────
const VERSION_TABLE_SIZE: usize = 1 << 20;
static VERSION_TABLE: [AtomicU64; VERSION_TABLE_SIZE] =
    [const { AtomicU64::new(0) }; VERSION_TABLE_SIZE];
static G_CLOCK: AtomicU64 = AtomicU64::new(1);
static THR_COUNTER: AtomicU64 = AtomicU64::new(1);
static G_TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);
// Commit lock serializes write-back to prevent version/data race
static COMMIT_LOCK: AtomicU64 = AtomicU64::new(0);

fn version_index(addr: usize) -> usize {
    (addr >> 3) & (VERSION_TABLE_SIZE - 1)
}

// ── Thread-local state ──────────────────────────────────────────
struct TxState {
    #[allow(dead_code)]
    id: u64,
    start_version: u64,
    read_only: bool,
    write_set: Vec<(usize, TypedValue)>,
    write_backs: Vec<WriteBack>,
}

thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}

fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    TX.with(|tx| {
        let mut b = tx.borrow_mut();
        f(b.as_mut().expect("TX not active"))
    })
}

fn tx_active() -> bool {
    TX.with(|tx| tx.borrow().is_some())
}

fn flush_tx() -> Option<Box<TxState>> {
    TX.with(|tx| tx.borrow_mut().take())
}

// ── Init ─────────────────────────────────────────────────────────
pub fn tm_init() {
    G_CLOCK.store(1, Ordering::Release);
    THR_COUNTER.store(1, Ordering::Release);
    for vt in VERSION_TABLE.iter() {
        vt.store(0, Ordering::Release);
    }
}

pub fn tm_exit() {}

pub fn tm_init_thread() {}
pub fn tm_exit_thread() {}

// ── Clock helpers ────────────────────────────────────────────────
fn get_clock() -> u64 {
    G_CLOCK.load(Ordering::Acquire)
}

fn increment_clock() -> u64 {
    G_CLOCK.fetch_add(1, Ordering::AcqRel) + 1
}

// ── Begin ────────────────────────────────────────────────────────
pub fn tm_begin() {
    let start = get_clock();
    let id = THR_COUNTER.fetch_add(1, Ordering::AcqRel);
    TX.with(|tx| {
        *tx.borrow_mut() = Some(Box::new(TxState {
            id,
            start_version: start,
            read_only: true,
            write_set: Vec::new(),
            write_backs: Vec::new(),
        }));
    });
}

// ── Abort ────────────────────────────────────────────────────────
pub fn tm_abort() {
    flush_tx();
}

pub fn tm_abort_count() -> u64 {
    G_TM_ABORT_COUNT.load(Ordering::Relaxed)
}

// ── Commit ───────────────────────────────────────────────────────
pub fn tm_commit() -> bool {
    let tx = match flush_tx() {
        Some(t) => t,
        None => return true,
    };
    fence(Ordering::SeqCst);

    if tx.read_only || tx.write_set.is_empty() {
        return true;
    }

    // Acquire commit lock (serializes write-back)
    while COMMIT_LOCK
        .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        std::hint::spin_loop();
    }

    // Validate: for each write address, check version table hasn't advanced
    // past our begin timestamp (no concurrent commit touched our addresses)
    for &(addr, _) in &tx.write_set {
        let idx = version_index(addr);
        let ver = VERSION_TABLE[idx].load(Ordering::Acquire);
        if ver > tx.start_version {
            COMMIT_LOCK.store(0, Ordering::Release);
            G_TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
            return false;
        }
    }

    // Increment clock for ordering
    let commit_version = increment_clock();

    // Write-back BEFORE updating version table (avoids version/data race)
    for wb in tx.write_backs {
        wb.apply();
    }
    fence(Ordering::SeqCst);

    // Update version table AFTER write-back
    for &(addr, _) in &tx.write_set {
        let idx = version_index(addr);
        VERSION_TABLE[idx].store(commit_version, Ordering::Release);
    }

    COMMIT_LOCK.store(0, Ordering::Release);
    true
}

// ── Read word ────────────────────────────────────────────────────
fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }

    // Check own write-set
    let ws_val = TX.with(|tx| {
        let b = tx.borrow();
        if let Some(ref t) = *b {
            for &(wa, ref wv) in t.write_set.iter().rev() {
                if wa == addr {
                    return Some(T::from_typed(wv));
                }
            }
        }
        None
    });
    if let Some(v) = ws_val {
        return v;
    }

    // Read from memory
    unsafe { (addr as *const T).read() }
}

// ── Write word ───────────────────────────────────────────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if !tx_active() {
        unsafe { (addr as *mut T).write(val); }
        return;
    }

    let tv = val.to_typed();
    with_tx(|tx| {
        tx.read_only = false;
        // Update existing write-set entry if present
        for entry in tx.write_set.iter_mut() {
            if entry.0 == addr {
                entry.1 = tv.clone();
                tx.write_backs.push(tv.into_write_back(addr));
                return;
            }
        }
        tx.write_set.push((addr, tv.clone()));
        tx.write_backs.push(tv.into_write_back(addr));
    });
}

// ── Typed read/write wrappers ────────────────────────────────────
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

pub fn tm_read_ptr<T>(addr: *mut *mut T) -> *mut T {
    let v = read_word::<u64>(addr as usize);
    v as *mut T
}
pub fn tm_write_ptr<T>(addr: *mut *mut T, val: *mut T) {
    write_word::<u64>(addr as usize, val as u64);
}

pub fn tm_read_raw(addr: *mut u8, dst: &mut [u8]) {
    for (i, d) in dst.iter_mut().enumerate() {
        *d = read_word::<u8>(addr as usize + i);
    }
}
pub fn tm_write_raw(addr: *mut u8, src: &[u8]) {
    for (i, &s) in src.iter().enumerate() {
        write_word::<u8>(addr as usize + i, s);
    }
}
