// ── TL2 TM backend for Rust ────────────────────────────
// Commit-time locking with a global commit lock.
// Uses a shared lock table (hash-based, 2^20 entries) and
// a global monotonically increasing clock.

use core::sync::atomic::{fence, AtomicU64, Ordering};
#[cfg(not(feature = "simulation"))]
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::OnceLock;
pub use runtime_core::{tm_install_tmx_hook, Primitive, TmxAbort, TypedValue, WriteBack};

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

#[allow(dead_code)]
fn try_lock(addr: usize) -> bool {
    lock_at_index(lock_index(addr)).try_lock_exclusive()
}

#[allow(dead_code)]
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
static COMMIT_LOCK: AtomicU64 = AtomicU64::new(0);
static G_CLOCK: AtomicU64 = AtomicU64::new(0);

pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);

#[cfg(feature = "stats")]
pub static TM_STATS: runtime_core::SyncCounters = runtime_core::SyncCounters::new();

// ── Thread-local / simulation state ──────────────────────
#[cfg(feature = "simulation")]
use std::sync::Mutex;

#[cfg(feature = "simulation")]
fn sim_tx_store() -> &'static Mutex<HashMap<u64, Option<Box<TxState>>>> {
    static STORE: OnceLock<Mutex<HashMap<u64, Option<Box<TxState>>>>> = OnceLock::new();
    STORE.get_or_init(|| Mutex::new(HashMap::new()))
}

// ── Transaction state ───────────────────────────────────
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone)]
pub struct TxState {
    read_set: Vec<(usize, u64)>,  // (addr, version_at_read)
    write_set: HashMap<usize, TypedValue>,
    #[allow(dead_code)]
    write_backs: Vec<WriteBack>,
    start_version: u64,
    #[allow(dead_code)]
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

// ── Thread-local storage ──────────────────────────────
#[cfg(not(feature = "simulation"))]
thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}

#[cfg(not(feature = "simulation"))]
fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    TX.with(|tx| {
        let mut b = tx.borrow_mut();
        f(b.as_mut().expect("no active transaction"))
    })
}

#[cfg(feature = "simulation")]
fn with_tx<R>(f: impl FnOnce(&mut TxState) -> R) -> R {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_tx_store();
    let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
    let state = map.get_mut(&tid).expect("no sim state for thread");
    f(state.as_mut().expect("no active transaction"))
}

#[cfg(not(feature = "simulation"))]
fn tx_active() -> bool {
    TX.with(|tx| tx.borrow().is_some())
}

#[cfg(feature = "simulation")]
fn tx_active() -> bool {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_tx_store();
    let map = store.lock().unwrap_or_else(|e| e.into_inner());
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
    let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
    map.get_mut(&tid).and_then(|s| s.take())
}

// ── Read word ───────────────────────────────────────────
fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }

    // Check write-set
    if let Some(tv) = with_tx(|tx| tx.write_set.get(&addr).cloned()) {
        return T::from_typed(&tv);
    }

    loop {
        while is_locked(addr) { std::hint::spin_loop(); }
        let ver = read_version(addr);
        let val: T = unsafe { (addr as *const T).read() };
        if read_version(addr) != ver { continue; }

        if with_tx(|tx| {
            if ver > tx.start_version {
                true
            } else {
                if tx.read_set.len() > 1_000_000 {
                    return true;
                }
                tx.read_set.push((addr, ver));
                #[cfg(feature = "stats")]
                TM_STATS.total_read_set_entries.fetch_add(1, Ordering::Relaxed);
                false
            }
        }) { std::panic::panic_any(TmxAbort); }
        return val;
    }
}

// ── Write word ──────────────────────────────────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if !tx_active() { unsafe { (addr as *mut T).write(val); } return; }

    let tv = val.to_typed();
    with_tx(|tx| {
        let is_new = tx.write_set.insert(addr, tv).is_none();
        #[cfg(feature = "stats")]
        if is_new { TM_STATS.total_write_set_entries.fetch_add(1, Ordering::Relaxed); }
    });
}

// ── Raw byte operations ─────────────────────────────────
fn read_raw_bytes(addr: usize, dst: &mut [u8]) {
    for (i, byte) in dst.iter_mut().enumerate() { *byte = read_word::<u8>(addr + i); }
}

fn write_raw_bytes(addr: usize, src: &[u8]) {
    fence(Ordering::SeqCst);
    if !tx_active() { unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len()); } return; }
    let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
    with_tx(|tx| {
        tx.write_set.insert(addr, tv);
    });
}

fn apply_typed_value(addr: usize, tv: &TypedValue) {
    unsafe {
        match tv {
            TypedValue::U8(v) => (addr as *mut u8).write(*v),
            TypedValue::U16(v) => (addr as *mut u16).write(*v),
            TypedValue::U32(v) => (addr as *mut u32).write(*v),
            TypedValue::U64(v) => (addr as *mut u64).write(*v),
            TypedValue::Bytes(b) => {
                std::ptr::copy_nonoverlapping(b.as_ptr(), addr as *mut u8, b.len());
            }
        }
    }
}

// ── Validate read-set (lock-based) ─────────────────────
fn validate_read_set(rs: &[(usize, u64)]) -> bool {
    #[cfg(feature = "stats")]
    TM_STATS.validations.fetch_add(1, Ordering::Relaxed);
    let ok = rs.iter().all(|(a, v)| read_version(*a) == *v);
    #[cfg(feature = "stats")]
    if !ok { TM_STATS.validation_failures.fetch_add(1, Ordering::Relaxed); }
    ok
}

// ── Commit ──────────────────────────────────────────────
pub fn tm_commit() -> bool {
    let tx = match flush_tx() { Some(t) => t, None => return true };
    fence(Ordering::SeqCst);

    if tx.write_set.is_empty() { return true; }

    // 1. Acquire global commit lock
    let my_id = 1;
    loop {
        let cur = COMMIT_LOCK.load(Ordering::Relaxed);
        if cur == 0 && COMMIT_LOCK.compare_exchange_weak(0, my_id, Ordering::Acquire, Ordering::Relaxed).is_ok() {
            break;
        }
        #[cfg(feature = "stats")]
        TM_STATS.lock_contentions.fetch_add(1, Ordering::Relaxed);
        std::hint::spin_loop();
    }

    // 2. Validate read-set
    if !validate_read_set(&tx.read_set) {
        COMMIT_LOCK.store(0, Ordering::Release);
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        #[cfg(feature = "stats")]
        TM_STATS.aborts.fetch_add(1, Ordering::Relaxed);
        return false;
    }

    // 3. Lock write-set addresses (sorted for deadlock freedom)
    let mut addrs: Vec<usize> = tx.write_set.keys().copied().collect();
    addrs.sort_unstable();
    let mut locked_idxs: Vec<usize> = Vec::with_capacity(addrs.len());
    for &a in &addrs {
        let idx = lock_index(a);
        if locked_idxs.last().copied() != Some(idx) {
            while !lock_at_index(idx).try_lock_exclusive() {
                #[cfg(feature = "stats")]
                TM_STATS.lock_acquire_failures.fetch_add(1, Ordering::Relaxed);
                std::hint::spin_loop();
            }
            locked_idxs.push(idx);
        }
    }
    fence(Ordering::SeqCst);

    // 4. Write-back from write-set
    for (addr, tv) in &tx.write_set {
        apply_typed_value(*addr, tv);
    }

    // 5. Unlock (reverse order)
    for &idx in locked_idxs.iter().rev() {
        lock_at_index(idx).unlock_exclusive();
    }
    fence(Ordering::SeqCst);

    // 6. Advance global clock
    G_CLOCK.fetch_add(1, Ordering::Release);

    // 7. Release global commit lock
    COMMIT_LOCK.store(0, Ordering::Release);

    #[cfg(feature = "stats")]
    TM_STATS.commits.fetch_add(1, Ordering::Relaxed);

    true
}

// ── Init ────────────────────────────────────────────────
pub fn tm_init() {
    tm_install_tmx_hook();
    locks();
    G_CLOCK.store(0, Ordering::Release);
    #[cfg(feature = "simulation")]
    for lock in locks().iter() {
        lock.data.store(0, Ordering::Release);
    }
    #[cfg(feature = "stats")]
    TM_STATS.reset();
}

pub fn tm_exit() {}

pub fn tm_init_thread() {
    #[cfg(not(feature = "simulation"))]
    TX.with(|tx| { *tx.borrow_mut() = None; });
    #[cfg(feature = "simulation")]
    {
        let tid = runtime_core::current_sim_thread_id();
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        map.entry(tid).or_insert(None);
    }
}

pub fn tm_exit_thread() {}

#[cfg(not(feature = "simulation"))]
pub fn tm_begin() {
    let sv = G_CLOCK.load(Ordering::Acquire);
    TX.with(|tx| { *tx.borrow_mut() = Some(Box::new(TxState::new(sv))); });
}

#[cfg(feature = "simulation")]
pub fn tm_begin() {
    let sv = G_CLOCK.load(Ordering::Acquire);
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_tx_store();
    let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
    *map.get_mut(&tid).expect("no sim state for thread") = Some(Box::new(TxState::new(sv)));
}

pub fn tm_abort() { flush_tx(); }

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

// ── Simulation-only API ──────────────────────────────────
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
        let map = store.lock().unwrap_or_else(|e| e.into_inner());
        map.clone()
    }

    pub fn restore_states(states: HashMap<u64, Option<Box<TxState>>>) {
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        *map = states;
    }

    pub fn reset() {
        let Some(tid) = runtime_core::try_current_sim_thread_id() else { return; };
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        map.remove(&tid);
    }

    #[cfg(feature = "stats")]
    pub fn take_stats() -> runtime_core::SyncCounters {
        let s = runtime_core::SyncCounters::new();
        s.validations.store(TM_STATS.validations.load(Ordering::Relaxed), Ordering::Relaxed);
        s.validation_failures.store(TM_STATS.validation_failures.load(Ordering::Relaxed), Ordering::Relaxed);
        s.lock_contentions.store(TM_STATS.lock_contentions.load(Ordering::Relaxed), Ordering::Relaxed);
        s.lock_acquire_failures.store(TM_STATS.lock_acquire_failures.load(Ordering::Relaxed), Ordering::Relaxed);
        s.total_read_set_entries.store(TM_STATS.total_read_set_entries.load(Ordering::Relaxed), Ordering::Relaxed);
        s.total_write_set_entries.store(TM_STATS.total_write_set_entries.load(Ordering::Relaxed), Ordering::Relaxed);
        s.commits.store(TM_STATS.commits.load(Ordering::Relaxed), Ordering::Relaxed);
        s.aborts.store(TM_STATS.aborts.load(Ordering::Relaxed), Ordering::Relaxed);
        TM_STATS.reset();
        s
    }

    #[cfg(feature = "stats")]
    pub fn print_stats(s: &runtime_core::SyncCounters) {
        use std::sync::atomic::Ordering;
        let val = s.validations.load(Ordering::Relaxed);
        let vfail = s.validation_failures.load(Ordering::Relaxed);
        let lcon = s.lock_contentions.load(Ordering::Relaxed);
        let laf  = s.lock_acquire_failures.load(Ordering::Relaxed);
        let trs  = s.total_read_set_entries.load(Ordering::Relaxed);
        let tws  = s.total_write_set_entries.load(Ordering::Relaxed);
        let com  = s.commits.load(Ordering::Relaxed);
        let abt  = s.aborts.load(Ordering::Relaxed);
        eprintln!("  STATS (TL2):");
        eprintln!("    Commits={}  Aborts={}  Val={}  VFail={}  CLock={}  LAqFail={}  RS={}  WS={}",
                  com, abt, val, vfail, lcon, laf, trs, tws);
    }
}
