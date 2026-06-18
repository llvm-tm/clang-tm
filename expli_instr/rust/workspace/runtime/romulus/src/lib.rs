// ── ROMULUS TM backend for Rust ──────────────────────────
// Version-table OCC with commit-lock serialisation.
// Read-validate protocol: capture version → read data → re-check.

#[cfg(not(feature = "simulation"))]
use std::cell::RefCell;
use std::sync::atomic::{fence, AtomicU64, Ordering};
use std::collections::HashMap;
#[cfg(feature = "simulation")]
use std::cell::UnsafeCell;

pub use runtime_core::{Primitive, TypedValue, WriteBack};

// ── SyncUnsafeCell: UnsafeCell that implements Sync ────
// Safe because simulation mode is single-threaded.
#[cfg(feature = "simulation")]
struct SyncUnsafeCell<T>(UnsafeCell<T>);
#[cfg(feature = "simulation")]
unsafe impl<T: Send> Sync for SyncUnsafeCell<T> {}
#[cfg(feature = "simulation")]
impl<T> SyncUnsafeCell<T> {
    fn new(val: T) -> Self { Self(UnsafeCell::new(val)) }
    fn get(&self) -> *mut T { self.0.get() }
}

// ── Globals ──────────────────────────────────────────────────────
static G_CLOCK: AtomicU64 = AtomicU64::new(1);
static THR_COUNTER: AtomicU64 = AtomicU64::new(1);
pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);
static COMMIT_LOCK: AtomicU64 = AtomicU64::new(0);

const VERSION_TABLE_SIZE: usize = 1 << 20;
static VERSION_TABLE: [AtomicU64; VERSION_TABLE_SIZE] =
    [const { AtomicU64::new(0) }; VERSION_TABLE_SIZE];

fn version_index(addr: usize) -> usize {
    (addr >> 3) & (VERSION_TABLE_SIZE - 1)
}

// ── TxState ─────────────────────────────────────────────────────
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone)]
pub struct TxState {
    pub timestamp: u64,
    pub read_only: bool,
    pub write_set: HashMap<usize, TypedValue>,
    pub write_backs: Vec<WriteBack>,
}

// ── Thread-local / simulation state ────────────────────────────
#[cfg(not(feature = "simulation"))]
thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}

#[cfg(feature = "simulation")]
fn sim_tx_store() -> &'static SyncUnsafeCell<HashMap<u64, Option<Box<TxState>>>> {
    use std::sync::OnceLock;
    static STORE: OnceLock<SyncUnsafeCell<HashMap<u64, Option<Box<TxState>>>>> = OnceLock::new();
    STORE.get_or_init(|| SyncUnsafeCell::new(HashMap::new()))
}

#[cfg(not(feature = "simulation"))]
fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    TX.with(|tx| {
        let mut b = tx.borrow_mut();
        f(b.as_mut().expect("TX not active"))
    })
}

#[cfg(feature = "simulation")]
fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_tx_store();
    let map = unsafe { &mut *store.get() };
    let state = map.get_mut(&tid).expect("no sim state for thread");
    f(state.as_mut().expect("no active transaction"))
}

#[cfg(not(feature = "simulation"))]
fn tx_active() -> bool {
    TX.with(|tx| tx.borrow().is_some())
}

#[cfg(feature = "simulation")]
fn tx_active() -> bool {
    let Some(tid) = runtime_core::try_current_sim_thread_id() else { return false; };
    let store = sim_tx_store();
    let map = unsafe { &*store.get() };
    map.get(&tid).map_or(false, |s| s.is_some())
}

#[cfg(not(feature = "simulation"))]
fn flush_tx() -> Option<Box<TxState>> {
    TX.with(|tx| tx.borrow_mut().take())
}

#[cfg(feature = "simulation")]
fn flush_tx() -> Option<Box<TxState>> {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_tx_store();
    let map = unsafe { &mut *store.get() };
    map.get_mut(&tid).and_then(|s| s.take())
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
    let ts = get_clock();
    THR_COUNTER.fetch_add(1, Ordering::AcqRel);
    let t = Box::new(TxState {
        timestamp: ts,
        read_only: true,
        write_set: HashMap::new(),
        write_backs: Vec::new(),
    });
    #[cfg(not(feature = "simulation"))]
    TX.with(|tx| { *tx.borrow_mut() = Some(t); });
    #[cfg(feature = "simulation")]
    {
        let tid = runtime_core::current_sim_thread_id();
        let store = sim_tx_store();
        let map = unsafe { &mut *store.get() };
        map.insert(tid, Some(t));
    }
}

// ── Abort ────────────────────────────────────────────────────────
pub fn tm_abort() {
    flush_tx();
}

pub fn tm_abort_count() -> u64 {
    TM_ABORT_COUNT.load(Ordering::Relaxed)
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

    // Acquire commit lock (serializes write-back, prevents version/data race)
    while COMMIT_LOCK
        .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        std::hint::spin_loop();
    }

    // Verify no version-table entry has changed since our snapshot
    for addr in tx.write_set.keys() {
        let idx = version_index(*addr);
        let ver = VERSION_TABLE[idx].load(Ordering::Acquire);
        if ver > tx.timestamp {
            COMMIT_LOCK.store(0, Ordering::Release);
            TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
            return false;
        }
    }

    // Increment clock for ordering
    let commit_ts = increment_clock();

    // Write-back BEFORE updating version table (avoids version/data race)
    for wb in tx.write_backs {
        wb.apply();
    }
    fence(Ordering::SeqCst);

    // Update version table AFTER write-back
    for addr in tx.write_set.keys() {
        let idx = version_index(*addr);
        VERSION_TABLE[idx].store(commit_ts, Ordering::Release);
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
    if let Some(tv) = with_tx(|tx| tx.write_set.get(&addr).cloned()) {
        return T::from_typed(&tv);
    }

    let val: T = unsafe { (addr as *const T).read() };
    val
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
        tx.write_set.insert(addr, tv.clone());
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

// ── Simulation-only API ─────────────────────────────────────────
#[cfg(feature = "simulation")]
pub mod sim {
    use super::*;

    pub fn set_thread_id(id: u64) {
        runtime_core::set_sim_thread_id(id);
    }

    pub fn clear_thread_id() {
        runtime_core::clear_sim_thread_id();
    }

    pub fn snapshot_states() -> HashMap<u64, Option<Box<TxState>>> {
        let store = sim_tx_store();
        let map = unsafe { &*store.get() };
        map.clone()
    }

    pub fn restore_states(states: HashMap<u64, Option<Box<TxState>>>) {
        let store = sim_tx_store();
        let map = unsafe { &mut *store.get() };
        *map = states;
    }

    pub fn reset() {
        let Some(tid) = runtime_core::try_current_sim_thread_id() else { return; };
        let store = sim_tx_store();
        let map = unsafe { &mut *store.get() };
        map.remove(&tid);
    }

    #[cfg(feature = "stats")]
    pub fn take_stats() -> runtime_core::SyncCounters {
        let s = runtime_core::SyncCounters::new();
        s.aborts.store(TM_ABORT_COUNT.load(Ordering::Relaxed), Ordering::Relaxed);
        TM_ABORT_COUNT.store(0, Ordering::Relaxed);
        s
    }

    #[cfg(feature = "stats")]
    pub fn print_stats(s: &runtime_core::SyncCounters) {
        use std::sync::atomic::Ordering;
        let abt = s.aborts.load(Ordering::Relaxed);
        eprintln!("  STATS (ROMULUS):");
        eprintln!("    Aborts={}", abt);
    }
}
