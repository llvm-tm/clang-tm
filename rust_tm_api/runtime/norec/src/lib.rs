// ── NOrec TM backend for Rust ───────────────────────────
// Value-based validation with a single global versioned lock.
// No lock table — reads go directly to memory with value-based
// conflict detection, writes buffer to a write-set and commit
// serializes via CAS on the global lock.

use core::sync::atomic::{fence, AtomicU64, Ordering};
use std::cell::RefCell;
pub use runtime_core::{tm_install_tmx_hook, Primitive, TmxAbort, TypedValue, WriteBack};

// ── Global lock ─────────────────────────────────────────
// Even = unlocked (version), Odd = locked.
// Version advances by 2 on each commit.
static GLOBAL_LOCK: AtomicU64 = AtomicU64::new(0);
static THR_COUNTER: AtomicU64 = AtomicU64::new(1);
pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);

// ── Read entry ─────────────────────────────────────────
struct ReadEntry {
    addr: usize,
    sz: u8,           // byte size of the read
    observed_val: u64, // value at read time, zero-extended to u64
}

// ── Write entry ─────────────────────────────────────────
struct WriteEntry {
    addr: usize,
    value: TypedValue,
}

// ── Transaction state ───────────────────────────────────
struct TxState {
    read_set: Vec<ReadEntry>,
    write_set: Vec<WriteEntry>,
    /// Deferred write-back closures (safe to apply at commit).
    write_backs: Vec<WriteBack>,
    snapshot: u64,
    read_only: bool,
    aborted: bool,
}

impl TxState {
    fn new(snapshot: u64) -> Self {
        TxState {
            read_set: Vec::with_capacity(64),
            write_set: Vec::with_capacity(8),
            write_backs: Vec::new(),
            snapshot,
            read_only: true,
            aborted: false,
        }
    }
}

// ── Thread-local state ──────────────────────────────────
thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}

// ── Helpers ─────────────────────────────────────────────
fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    TX.with(|tx| {
        let mut b = tx.borrow_mut();
        f(b.as_mut().expect("no active transaction"))
    })
}

fn tx_active() -> bool {
    TX.with(|tx| tx.borrow().is_some())
}

fn flush_tx() -> Option<Box<TxState>> {
    TX.with(|tx| tx.borrow_mut().take())
}

fn read_mem_val(addr: usize, sz: u8) -> u64 {
    unsafe {
        match sz {
            1 => (addr as *const u8).read() as u64,
            2 => (addr as *const u16).read() as u64,
            4 => (addr as *const u32).read() as u64,
            8 => (addr as *const u64).read(),
            _ => 0,
        }
    }
}

fn byte_size_of_tv(tv: &TypedValue) -> u8 {
    match tv {
        TypedValue::U8(_) => 1,
        TypedValue::U16(_) => 2,
        TypedValue::U32(_) => 4,
        TypedValue::U64(_) => 8,
        TypedValue::Bytes(b) => b.len() as u8,
    }
}

// ── Value-based validation ──────────────────────────────
// Re-reads every address in the read-set from memory and
// compares to the observed value.  Returns the current clock
// value if all match.
fn validate_impl(tx: &mut TxState) -> Option<u64> {
    loop {
        let time = GLOBAL_LOCK.load(Ordering::Acquire);
        if time & 1 != 0 {
            std::hint::spin_loop();
            continue;
        }
        for r in &tx.read_set {
            let cur = read_mem_val(r.addr, r.sz);
            if cur != r.observed_val {
                return None;
            }
        }
        if time == GLOBAL_LOCK.load(Ordering::Acquire) {
            return Some(time);
        }
    }
}

// ── Read word ───────────────────────────────────────────
fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() {
        // Outside any TX — raw read
        return unsafe { (addr as *const T).read() };
    }
    let sz = core::mem::size_of::<T>() as u8;

    // Phase 1: check our own write-set first (reverse scan)
    let ws_val = TX.with(|tx| {
        let b = tx.borrow();
        let t = b.as_ref().unwrap();
        for e in t.write_set.iter().rev() {
            if e.addr == addr {
                let esz = byte_size_of_tv(&e.value);
                if esz == sz {
                    return Some(T::from_typed(&e.value));
                }
                // Type interchange fallback
            }
        }
        None
    });
    if let Some(v) = ws_val {
        return v;
    }

    // Phase 2: memory read with validation loop
    loop {
        let val_u64 = read_mem_val(addr, sz);
        let val = unsafe { (addr as *const T).read() };

        let snapshot = with_tx(|tx| tx.snapshot);
        let gl = GLOBAL_LOCK.load(Ordering::Acquire);

        if gl == snapshot {
            // Clock stable — install read-set entry
            with_tx(|tx| {
                tx.read_set.push(ReadEntry {
                    addr,
                    sz,
                    observed_val: val_u64,
                });
            });
            return val;
        }

        // Clock changed — validate all reads
        with_tx(|tx| {
            match validate_impl(tx) {
                Some(s) => tx.snapshot = s,
                None => std::panic::panic_any(TmxAbort),
            }
        });
        // Loop back to re-read this address with the new snapshot
    }
}

// ── Write word ──────────────────────────────────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if !tx_active() {
        unsafe { (addr as *mut T).write(val); }
        return;
    }

    let tv = val.to_typed();
    let sz = byte_size_of_tv(&tv);

    with_tx(|tx| {
        tx.read_only = false;
        tx.write_backs.push(tv.clone().into_write_back(addr));

        // Scan from end for existing entry at this address
        for i in (0..tx.write_set.len()).rev() {
            if tx.write_set[i].addr == addr {
                let esz = byte_size_of_tv(&tx.write_set[i].value);
                if esz == sz {
                    // Same size: update in-place
                    tx.write_set[i].value = tv;
                    return;
                }
                if esz >= sz {
                    // Wider entry already covers this — skip
                    return;
                }
                // Narrower entry — fall through to add new entry
                break;
            }
        }

        tx.write_set.push(WriteEntry { addr, value: tv });
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
    if !tx_active() {
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len());
        }
        return;
    }

    let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
    with_tx(|tx| {
        tx.read_only = false;
        tx.write_backs.push(tv.clone().into_write_back(addr));
        // Remove/existing entry at this addr for raw bytes
        tx.write_set.retain(|e| e.addr != addr);
        tx.write_set.push(WriteEntry { addr, value: tv });
    });
}

// ── Public API ──────────────────────────────────────────
pub fn tm_init() {
    tm_install_tmx_hook();
    GLOBAL_LOCK.store(0, Ordering::Release);
    THR_COUNTER.store(1, Ordering::Release);
}

pub fn tm_exit() {}

pub fn tm_init_thread() {
    TX.with(|tx| {
        *tx.borrow_mut() = None;
    });
}

pub fn tm_exit_thread() {}

pub fn tm_begin() {
    // Wait for snapshot to be even (unlocked)
    let snap = loop {
        let v = GLOBAL_LOCK.load(Ordering::Acquire);
        if v & 1 == 0 {
            break v;
        }
        std::hint::spin_loop();
    };
    TX.with(|tx| {
        *tx.borrow_mut() = Some(Box::new(TxState::new(snap)));
    });
}

pub fn tm_commit() -> bool {
    let mut tx = match flush_tx() {
        Some(t) => t,
        None => return true,
    };
    fence(Ordering::SeqCst);

    // Read-only fast path
    if tx.read_only || tx.write_set.is_empty() {
        return true;
    }

    let mut snapshot = tx.snapshot;

    // Acquire global lock via CAS
    loop {
        let expect = snapshot;
        let desire = snapshot + 1; // odd = locked
        if GLOBAL_LOCK
            .compare_exchange_weak(expect, desire, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            break;
        }
        // CAS failed — validate and retry
        snapshot = match validate_impl(&mut tx) { Some(s) => s, None => { TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed); return false; } };
    }

    // We hold the global lock. Write-back all entries.
    for wb in tx.write_backs {
        wb.apply();
    }

    // Release lock and advance version (even → next even)
    GLOBAL_LOCK.store(snapshot + 2, Ordering::Release);

    true
}

pub fn tm_abort() {
    flush_tx();
}

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
    let v = read_word::<u64>(addr as usize);
    v as *mut T
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
