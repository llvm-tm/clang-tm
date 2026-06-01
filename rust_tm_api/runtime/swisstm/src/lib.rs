// ── SwissTM TM backend for Rust ─────────────────────────
// Eager-locking with orecs (ownership records) and undo log.
// Reads record orec version; writes acquire orec exclusive and
// write through to memory, saving old value in undo log.

use core::sync::atomic::{fence, AtomicU64, Ordering};
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::OnceLock;
pub use runtime_core::{Primitive, TypedValue};

// ── Constants ───────────────────────────────────────────
const OWNED_BIT: u64 = 0;
const VERSION_SHIFT: u64 = 8;
const TABLE_BITS: u64 = 20;
const TABLE_SIZE: usize = 1 << TABLE_BITS;

fn lock_index(addr: usize) -> usize {
    let h = (addr as u64).wrapping_mul(0x9E3779B97F4A7C15);
    (h >> (64 - TABLE_BITS)) as usize
}

// ── Orec ────────────────────────────────────────────────
struct Orec {
    state: AtomicU64, // bit 0 = owned, bits 8+ = version
}

impl Orec {
    const fn new() -> Self {
        Orec { state: AtomicU64::new(0) }
    }
    fn is_owned(&self) -> bool {
        self.state.load(Ordering::Relaxed) & 1 != 0
    }
    fn version(&self) -> u64 {
        self.state.load(Ordering::Acquire) >> VERSION_SHIFT
    }
    fn read_version_preserving(&self) -> u64 {
        // Load the state and extract version, preserving read-lock bits
        let s = self.state.load(Ordering::Acquire);
        s >> VERSION_SHIFT
    }
    fn try_lock_exclusive(&self, tx_id: u64) -> bool {
        let cur = self.state.load(Ordering::Relaxed);
        if cur & 1 != 0 { return false; }
        // Set owned bit, preserve version and incarnation bits
        let desired = cur | 1;
        self.state.compare_exchange_weak(cur, desired, Ordering::Acquire, Ordering::Relaxed).is_ok()
    }
    fn unlock_exclusive_and_inc_version(&self) {
        let cur = self.state.load(Ordering::Relaxed);
        let ver = (cur >> VERSION_SHIFT) + 1;
        self.state.store(ver << VERSION_SHIFT, Ordering::Release);
    }
    fn unlock_exclusive_with_version(&self, ver: u64) {
        self.state.store(ver << VERSION_SHIFT, Ordering::Release);
    }
}

fn orec_for(addr: usize) -> &'static Orec {
    &orecs()[lock_index(addr)]
}

static OREC_TABLE: OnceLock<Box<[Orec]>> = OnceLock::new();

fn orecs() -> &'static [Orec] {
    OREC_TABLE.get_or_init(|| {
        (0..TABLE_SIZE).map(|_| Orec::new()).collect::<Vec<_>>().into_boxed_slice()
    })
}

// ── Global clock ────────────────────────────────────────
static G_CLOCK: AtomicU64 = AtomicU64::new(0);

pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);

// ── Transaction state ───────────────────────────────────
struct TxState {
    read_set: Vec<(usize, u64)>,        // (addr, observed_version)
    write_set: HashMap<usize, TypedValue>,
    undo_log: Vec<(usize, u64, u8)>,    // (addr, old_val_u64, byte_size)
    locked_orecs: Vec<usize>,           // orec indices locked for writing
    start_version: u64,
    aborted: bool,
    tx_id: u64,
}

impl TxState {
    fn new(start_version: u64, tx_id: u64) -> Self {
        TxState {
            read_set: Vec::with_capacity(64),
            write_set: HashMap::with_capacity(8),
            undo_log: Vec::new(),
            locked_orecs: Vec::new(),
            start_version,
            aborted: false,
            tx_id,
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

// ── TX ID generator ─────────────────────────────────────
static THR_COUNTER: AtomicU64 = AtomicU64::new(1);

// ── Memory helpers ─────────────────────────────────────
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

fn write_mem_u64(addr: usize, val: u64, sz: u8) {
    unsafe {
        match sz {
            1 => (addr as *mut u8).write(val as u8),
            2 => (addr as *mut u16).write(val as u16),
            4 => (addr as *mut u32).write(val as u32),
            8 => (addr as *mut u64).write(val),
            _ => {}
        }
    }
}

fn write_mem_typed(addr: usize, tv: &TypedValue) {
    unsafe {
        match tv {
            TypedValue::U8(v) => (addr as *mut u8).write(*v),
            TypedValue::U16(v) => (addr as *mut u16).write(*v),
            TypedValue::U32(v) => (addr as *mut u32).write(*v),
            TypedValue::U64(v) => (addr as *mut u64).write(*v),
            TypedValue::Bytes(b) => {
                let dst = addr as *mut u8;
                for (i, &byte) in b.iter().enumerate() { dst.add(i).write(byte); }
            }
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

// ── Read word ───────────────────────────────────────────
fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }

    let sz = core::mem::size_of::<T>() as u8;

    // Check write-set
    if let Some(tv) = TX.with(|tx| {
        tx.borrow().as_ref().and_then(|t| t.write_set.get(&addr).cloned())
    }) { return T::from_typed(&tv); }

    loop {
        if tx_aborted() {
            return unsafe { (addr as *const T).read() };
        }
        let idx = lock_index(addr);
        let orec = orec_for(addr);

        // If another TX owns this orec, abort (don't spin — eager-locking
        // protocol means the holder may be delayed for the entire TX)
        if orec.is_owned() {
            with_tx(|tx| { tx.aborted = true; });
            return unsafe { (addr as *const T).read() };
        }

        let ver = orec.version();
        let val: T = unsafe { (addr as *const T).read() };

        // Re-check orec
        if orec.is_owned() || orec.version() != ver { continue; }

        let aborted = with_tx(|tx| {
            // Check that the orec wasn't write-locked by someone since we read
            // (we already checked: not owned before, not owned after, version same)
            if ver > tx.start_version {
                tx.aborted = true;
                true
            } else {
                tx.read_set.push((addr, ver));
                false
            }
        });
        if aborted { return val; }
        return val;
    }
}

// ── Write word (write-through + undo log) ──────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if tx_aborted() { return; }
    if !tx_active() { unsafe { (addr as *mut T).write(val); } return; }

    let tv = val.to_typed();
    let sz = byte_size_of_tv(&tv);

    with_tx(|tx| {
        // Check if we already own this address's orec
        let idx = lock_index(addr);
        if !tx.locked_orecs.contains(&idx) {
            // Acquire the orec exclusively (bounded spin, then abort)
            let orec = orec_for(addr);
            let mut locked = false;
            for _ in 0..5000 {
                if orec.try_lock_exclusive(tx.tx_id) {
                    tx.locked_orecs.push(idx);
                    locked = true;
                    break;
                }
                std::hint::spin_loop();
            }
            if !locked { tx.aborted = true; return; }
        }

        // Save old value in undo log (first write only)
        if !tx.write_set.contains_key(&addr) {
            let old_val = read_mem_val(addr, sz);
            tx.undo_log.push((addr, old_val, sz));
        }

        // Write through to memory
        write_mem_typed(addr, &tv);
        tx.write_set.insert(addr, tv);
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

    with_tx(|tx| {
        let idx = lock_index(addr);
        if !tx.locked_orecs.contains(&idx) {
            let orec = orec_for(addr);
            let mut spins = 0;
            while !orec.try_lock_exclusive(tx.tx_id) {
                spins += 1;
                if spins > 10000 { tx.aborted = true; return; }
                std::hint::spin_loop();
            }
            tx.locked_orecs.push(idx);
        }

        // Save old values byte-by-byte
        for (i, _) in src.iter().enumerate() {
            let byte_addr = addr + i;
            if !tx.write_set.contains_key(&byte_addr) {
                let old_val = unsafe { (byte_addr as *const u8).read() };
                tx.undo_log.push((byte_addr, old_val as u64, 1));
            }
        }

        let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
        unsafe {
            let dst = addr as *mut u8;
            for (i, &byte) in src.iter().enumerate() { dst.add(i).write(byte); }
        }
        tx.write_set.insert(addr, tv);
    });
}

// ── Undo restore ───────────────────────────────────────
fn undo_restore(tx: &TxState) {
    for &(addr, old_val, sz) in tx.undo_log.iter().rev() {
        write_mem_u64(addr, old_val, sz);
    }
}

// ── Validate read-set ──────────────────────────────────
fn validate(tx: &TxState) -> bool {
    for &(addr, observed_ver) in &tx.read_set {
        let cur_ver = orec_for(addr).version();
        if cur_ver != observed_ver {
            return false;
        }
    }
    true
}

// ── Commit ──────────────────────────────────────────────
pub fn tm_commit() -> bool {
    let tx = match flush_tx() { Some(t) => t, None => return true };
    fence(Ordering::SeqCst);

    if tx.aborted {
        undo_restore(&tx);
        for &idx in &tx.locked_orecs {
            orecs()[idx].unlock_exclusive_and_inc_version();
        }
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        // Exponential backoff to avoid eager-locking livelock
        let ac = TM_ABORT_COUNT.load(Ordering::Relaxed) & 0x3F;
        for _ in 0..(ac + 1) * 100 { std::hint::spin_loop(); }
        return false;
    }

    if tx.write_set.is_empty() {
        return true;
    }

    // Validate read-set
    if !validate(&tx) {
        undo_restore(&tx);
        for &idx in &tx.locked_orecs {
            orecs()[idx].unlock_exclusive_and_inc_version();
        }
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        let ac = TM_ABORT_COUNT.load(Ordering::Relaxed) & 0x3F;
        for _ in 0..(ac + 1) * 100 { std::hint::spin_loop(); }
        return false;
    }

    // Release locks with incremented version
    let commit_ver = G_CLOCK.fetch_add(1, Ordering::Release) + 1;
    for &idx in &tx.locked_orecs {
        orecs()[idx].unlock_exclusive_with_version(commit_ver);
    }

    true
}

// ── Init ────────────────────────────────────────────────
pub fn tm_init() {
    orecs();
    G_CLOCK.store(0, Ordering::Release);
}

pub fn tm_exit() {}

pub fn tm_init_thread() {
    // Assign TX ID for locking ownership check
    let tid = THR_COUNTER.fetch_add(1, Ordering::Relaxed);
    TX.with(|tx| {
        *tx.borrow_mut() = Some(Box::new(TxState::new(0, tid)));
    });
}

pub fn tm_exit_thread() {
    TX.with(|tx| { *tx.borrow_mut() = None; });
}

pub fn tm_begin() {
    let sv = G_CLOCK.load(Ordering::Acquire);
    let tid = THR_COUNTER.load(Ordering::Relaxed);
    TX.with(|tx| {
        *tx.borrow_mut() = Some(Box::new(TxState::new(sv, tid)));
    });
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
