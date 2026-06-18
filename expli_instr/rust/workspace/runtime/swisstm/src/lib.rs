// ── SwissTM TM backend for Rust ─────────────────────────
// Eager-locking with orecs (ownership records) and undo log.
// Reads record orec version; writes acquire orec exclusive and
// write through to memory, saving old value in undo log.

use core::sync::atomic::{fence, AtomicBool, AtomicU64, Ordering};
use std::collections::HashMap;
#[cfg(not(feature = "simulation"))]
use std::cell::RefCell;
#[cfg(feature = "simulation")]
use std::cell::UnsafeCell;
use std::sync::OnceLock;
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

// ── Constants ───────────────────────────────────────────
const VERSION_SHIFT: u64 = 8;
const TABLE_BITS: u64 = 20;
const TABLE_SIZE: usize = 1 << TABLE_BITS;
const WN_THRESHOLD: u64 = 10;     // writes before we get a cm_ts priority

fn lock_index(addr: usize) -> usize {
    let h = (addr as u64).wrapping_mul(0x9E3779B97F4A7C15);
    (h >> (64 - TABLE_BITS)) as usize
}

// ── Orec ────────────────────────────────────────────────
// Two-field design (like C++ SwissTM):
//   version_lock: bit 0 = READ_LOCKED (only during commit Phase 1, microseconds),
//                 bits 8+ = version (stable across TX body, bumped at commit)
//   w_lock: writer-exclusive (invisible to readers — readers never wait on w_lock)
//   owner_cm_ts: contention-management ts of owning TX (set when w_lock held)
struct Orec {
    version_lock: AtomicU64,
    w_lock: AtomicBool,
    owner_cm_ts: AtomicU64,
}

impl Orec {
    const fn new() -> Self {
        Orec {
            version_lock: AtomicU64::new(0),
            w_lock: AtomicBool::new(false),
            owner_cm_ts: AtomicU64::new(0),
        }
    }
    // Reader-visible version (bits 8+ of version_lock).  Never blocked by w_lock.
    fn version(&self) -> u64 {
        self.version_lock.load(Ordering::Acquire) >> VERSION_SHIFT
    }
    // Acquire the writer lock.  Does NOT affect version_lock — readers can still
    // read the version and will accept dirty values (validated at commit).
    fn try_lock_exclusive_with_cm(&self, cm_ts: u64) -> bool {
        if self.w_lock.load(Ordering::Relaxed) { return false; }
        if self.w_lock.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_ok() {
            self.owner_cm_ts.store(cm_ts, Ordering::Release);
            true
        } else {
            false
        }
    }
    fn read_owner_cm_ts(&self) -> u64 {
        self.owner_cm_ts.load(Ordering::Acquire)
    }
    // Release w_lock only — no version bump (no read_set tracking for
    // dirty reads, so bumping would only cause cascading aborts).
    fn unlock_wlock(&self) {
        self.w_lock.store(false, Ordering::Release);
    }
    // Commit Phase 1: signal readers that we're about to validate+commit.
    fn set_read_locked(&self) {
        self.version_lock.fetch_or(1, Ordering::Release);
    }
    // Commit Phase 5: store new version, clear READ_LOCKED bit, release w_lock.
    fn commit_release(&self, ver: u64) {
        self.version_lock.store(ver << VERSION_SHIFT, Ordering::Release);
        self.w_lock.store(false, Ordering::Release);
    }
    // Abort after Phase 1: clear READ_LOCKED + release w_lock (no version bump).
    fn abort_release(&self) {
        let cur = self.version_lock.load(Ordering::Relaxed);
        self.version_lock.store(cur & !1, Ordering::Release);
        self.w_lock.store(false, Ordering::Release);
    }
}

// ── Global contention-management clock ──────────────────
static GREEDY_TS: AtomicU64 = AtomicU64::new(0);

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
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone)]
pub struct TxState {
    pub read_set: Vec<(usize, u64)>,        // (addr, observed_version)
    pub write_set: HashMap<usize, TypedValue>,
    /// Deferred undo closures (safe to apply on rollback).
    pub undo_backs: Vec<WriteBack>,
    pub locked_orecs: Vec<usize>,           // orec indices locked for writing
    pub valid_ts: u64,                      // highest commit_ts for which read_set is validated
    pub cm_ts: u64,                         // contention-management timestamp (u64::MAX = none)
    pub write_count: u64,                   // writes performed in this TX (for cm timestamp)
    #[allow(dead_code)]
    pub succ_abort_count: u64,              // consecutive aborts (for backoff)
    #[allow(dead_code)]
    pub aborted: bool,
    #[allow(dead_code)]
    pub tx_id: u64,
}

impl TxState {
    fn new(global_clock: u64, tx_id: u64, prev_abort_count: u64) -> Self {
        TxState {
            read_set: Vec::with_capacity(64),
            write_set: HashMap::with_capacity(8),
            undo_backs: Vec::new(),
            locked_orecs: Vec::new(),
            valid_ts: global_clock,
            cm_ts: u64::MAX,
            write_count: 0,
            succ_abort_count: prev_abort_count,
            aborted: false,
            tx_id,
        }
    }
}

// ── Contention manager ──────────────────────────────────
// Priority = lower cm_ts wins (older TX).  A TX gets a cm_ts
// from GREEDY_TS after WN_THRESHOLD writes.
fn cm_on_write(tx: &mut TxState) {
    if tx.cm_ts == u64::MAX && tx.write_count >= WN_THRESHOLD {
        tx.cm_ts = GREEDY_TS.fetch_add(1, Ordering::SeqCst);
    }
}

// Return true if we should abort (owner has higher priority).
fn cm_should_abort(tx: &TxState, orec: &Orec, spin_count: &mut u32) -> bool {
    if tx.cm_ts == u64::MAX {
        let owner_cm = orec.read_owner_cm_ts();
        if owner_cm == u64::MAX {
            // Both pre-WN_THRESHOLD: bounded spin
            *spin_count += 1;
            return *spin_count >= 100000;
        }
        return true;  // Owner earned priority, we should abort
    }
    let owner_cm = orec.read_owner_cm_ts();
    if owner_cm < tx.cm_ts {
        return true;
    }
    false  // We have higher priority, keep waiting
}

fn cm_backoff(abort_count: u64) {
    let ac = abort_count & 0x3F;
    let delay_us = 50u64 * (1u64 << (ac.min(7) as u64));
    if delay_us <= 200 {
        #[cfg(not(feature = "simulation"))]
        for _ in 0..delay_us * 2000 {
            std::hint::spin_loop();
        }
        #[cfg(feature = "simulation")]
        std::thread::sleep(std::time::Duration::from_micros(delay_us));
    } else {
        std::thread::sleep(std::time::Duration::from_micros(delay_us));
    }
}

// ── Thread-local / simulation state ────────────────────
#[cfg(feature = "simulation")]
struct SimThreadState {
    tx: Option<Box<TxState>>,
    succ_abort_count: u64,
}

#[cfg(not(feature = "simulation"))]
thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
    static SUCC_ABORT_COUNT: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

#[cfg(feature = "simulation")]
fn sim_state_store() -> &'static SyncUnsafeCell<HashMap<u64, SimThreadState>> {
    static STORE: OnceLock<SyncUnsafeCell<HashMap<u64, SimThreadState>>> = OnceLock::new();
    STORE.get_or_init(|| SyncUnsafeCell::new(HashMap::new()))
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
    let store = sim_state_store();
    let map = unsafe { &mut *store.get() };
    let st = map.get_mut(&tid).expect("no sim state for thread");
    f(st.tx.as_mut().expect("no active transaction"))
}

#[cfg(not(feature = "simulation"))]
fn tx_abort_count() -> u64 {
    SUCC_ABORT_COUNT.with(|c| c.get())
}

#[cfg(feature = "simulation")]
fn tx_abort_count() -> u64 {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_state_store();
    let map = unsafe { &*store.get() };
    map.get(&tid).map_or(0, |st| st.succ_abort_count)
}

#[cfg(not(feature = "simulation"))]
fn inc_tx_abort_count() {
    SUCC_ABORT_COUNT.with(|c| c.set(c.get() + 1));
}

#[cfg(feature = "simulation")]
fn inc_tx_abort_count() {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_state_store();
    let map = unsafe { &mut *store.get() };
    if let Some(st) = map.get_mut(&tid) {
        st.succ_abort_count += 1;
    }
}

#[cfg(not(feature = "simulation"))]
fn reset_tx_abort_count() {
    SUCC_ABORT_COUNT.with(|c| c.set(0));
}

#[cfg(feature = "simulation")]
fn reset_tx_abort_count() {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_state_store();
    let map = unsafe { &mut *store.get() };
    if let Some(st) = map.get_mut(&tid) {
        st.succ_abort_count = 0;
    }
}

#[cfg(not(feature = "simulation"))]
fn tx_active() -> bool {
    TX.with(|tx| match *tx.borrow() { Some(ref t) => !t.aborted, None => false })
}

#[cfg(feature = "simulation")]
fn tx_active() -> bool {
    let Some(tid) = runtime_core::try_current_sim_thread_id() else { return false; };
    let store = sim_state_store();
    let map = unsafe { &*store.get() };
    map.get(&tid).map_or(false, |st| {
        st.tx.as_ref().map_or(false, |t| !t.aborted)
    })
}

#[cfg(not(feature = "simulation"))]
fn tx_aborted() -> bool {
    TX.with(|tx| match *tx.borrow() { Some(ref t) => t.aborted, None => false })
}

#[cfg(feature = "simulation")]
fn tx_aborted() -> bool {
    let Some(tid) = runtime_core::try_current_sim_thread_id() else { return false; };
    let store = sim_state_store();
    let map = unsafe { &*store.get() };
    map.get(&tid).map_or(false, |st| {
        st.tx.as_ref().map_or(false, |t| t.aborted)
    })
}

#[cfg(not(feature = "simulation"))]
fn flush_tx() -> Option<Box<TxState>> {
    TX.with(|tx| tx.borrow_mut().take())
}

#[cfg(feature = "simulation")]
fn flush_tx() -> Option<Box<TxState>> {
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_state_store();
    let map = unsafe { &mut *store.get() };
    map.get_mut(&tid).and_then(|st| st.tx.take())
}

// ── TX ID generator ─────────────────────────────────────
#[cfg(not(feature = "simulation"))]
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
        if tx_aborted() { return unsafe { core::mem::zeroed() }; }
        return unsafe { (addr as *const T).read() };
    }

    // Check write-set
    if let Some(tv) = with_tx(|tx| tx.write_set.get(&addr).cloned()) {
        return T::from_typed(&tv);
    }

    loop {
        if tx_aborted() {
            return unsafe { (addr as *const T).read() };
        }
        let orec = orec_for(addr);

        if orec.w_lock.load(Ordering::Relaxed) {
            let owner_cm = orec.read_owner_cm_ts();
            if owner_cm == u64::MAX {
                #[cfg(not(feature = "simulation"))]
                {
                    // Light TX (<10 writes): spin briefly
                    for _ in 0..2000000 {
                        if tx_aborted() { return unsafe { core::mem::zeroed() }; }
                        if !orec.w_lock.load(Ordering::Relaxed) { break; }
                        std::hint::spin_loop();
                    }
                    if !orec.w_lock.load(Ordering::Relaxed) {
                        let ver = orec.version();
                        let val: T = unsafe { (addr as *const T).read() };
                        with_tx(|tx| { tx.read_set.push((addr, ver)); });
                        return val;
                    }
                }
                #[cfg(feature = "simulation")]
                {
                    // Can't spin in simulation — abort
                    with_tx(|tx| tx.aborted = true);
                    return unsafe { core::mem::zeroed() };
                }
            }
            return unsafe { (addr as *const T).read() };
        }

        // Spin while commit Phase 1 has version_lock READ_LOCKED
        #[cfg(not(feature = "simulation"))]
        while orec.version_lock.load(Ordering::Relaxed) & 1 != 0 {
            if tx_aborted() { return unsafe { core::mem::zeroed() }; }
            std::hint::spin_loop();
        }
        #[cfg(feature = "simulation")]
        if orec.version_lock.load(Ordering::Relaxed) & 1 != 0 {
            with_tx(|tx| tx.aborted = true);
            return unsafe { core::mem::zeroed() };
        }

        let ver = orec.version();
        let val: T = unsafe { (addr as *const T).read() };

        // Re-check: if READ_LOCKED now set, or version changed, retry.
        if orec.version_lock.load(Ordering::Acquire) != (ver << VERSION_SHIFT) { continue; }

        let aborted = with_tx(|tx| {
            if ver > tx.valid_ts && !extend(tx) {
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
        let idx = lock_index(addr);
        if !tx.locked_orecs.contains(&idx) {
            let orec = orec_for(addr);
            let mut spin_count = 0u32;
            loop {
                if orec.try_lock_exclusive_with_cm(tx.cm_ts) {
                    tx.locked_orecs.push(idx);
                    break;
                }
                if cm_should_abort(tx, orec, &mut spin_count) {
                    tx.aborted = true;
                    return;
                }
                #[cfg(not(feature = "simulation"))]
                std::hint::spin_loop();
                #[cfg(feature = "simulation")]
                {
                    tx.aborted = true;
                    return;
                }
            }

            if orec.version() > tx.valid_ts && !extend(tx) {
                tx.aborted = true;
                return;
            }
        }

        // Save old value in undo log (first write only)
        if !tx.write_set.contains_key(&addr) {
            let old_val = read_mem_val(addr, sz);
            let wb = match sz {
                1 => WriteBack::U8(addr, old_val as u8),
                2 => WriteBack::U16(addr, old_val as u16),
                4 => WriteBack::U32(addr, old_val as u32),
                _ => WriteBack::U64(addr, old_val),
            };
            tx.undo_backs.push(wb);
        }

        write_mem_typed(addr, &tv);
        tx.write_set.insert(addr, tv);
        tx.write_count += 1;
        cm_on_write(tx);
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
            let mut spin_count = 0u32;
            loop {
                if orec.try_lock_exclusive_with_cm(tx.cm_ts) {
                    tx.locked_orecs.push(idx);
                    break;
                }
                if cm_should_abort(tx, orec, &mut spin_count) {
                    tx.aborted = true;
                    return;
                }
                #[cfg(not(feature = "simulation"))]
                std::hint::spin_loop();
                #[cfg(feature = "simulation")]
                {
                    tx.aborted = true;
                    return;
                }
            }
        }

        for (i, _) in src.iter().enumerate() {
            let byte_addr = addr + i;
            if !tx.write_set.contains_key(&byte_addr) {
                let old_val = unsafe { (byte_addr as *const u8).read() };
                tx.undo_backs.push(old_val.to_typed().into_write_back(byte_addr));
            }
        }

        let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
        unsafe {
            let dst = addr as *mut u8;
            for (i, &byte) in src.iter().enumerate() { dst.add(i).write(byte); }
        }
        tx.write_set.insert(addr, tv);
        tx.write_count += 1;
        cm_on_write(tx);
    });
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

// ── Extend (validate + advance valid_ts) ────────────────
fn extend(tx: &mut TxState) -> bool {
    let ts = G_CLOCK.load(Ordering::Acquire);
    if ts > tx.valid_ts {
        if validate(tx) {
            tx.valid_ts = ts;
            return true;
        }
    }
    false
}

// ── Abort cleanup ───────────────────────────────────────
pub fn tm_abort() {
    if let Some(tx) = flush_tx() {
        for u in tx.undo_backs { u.apply(); }
        for &idx in &tx.locked_orecs {
            orecs()[idx].unlock_wlock();
        }
    }
}

// ── Commit ──────────────────────────────────────────────
pub fn tm_commit() -> bool {
    let tx = match flush_tx() { Some(t) => t, None => return true };
    fence(Ordering::SeqCst);

    if tx.aborted {
        for u in tx.undo_backs { u.apply(); }
        for &idx in &tx.locked_orecs {
            orecs()[idx].unlock_wlock();
        }
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        inc_tx_abort_count();
        let ac = TM_ABORT_COUNT.load(Ordering::Relaxed);
        cm_backoff(ac);
        return false;
    }

    if tx.write_set.is_empty() {
        reset_tx_abort_count();
        return true;
    }

    // Phase 1: set READ_LOCKED to block new readers during validation
    for &idx in &tx.locked_orecs {
        orecs()[idx].set_read_locked();
    }
    fence(Ordering::SeqCst);

    // Phase 4: validate read-set
    if !validate(&tx) {
        for u in tx.undo_backs { u.apply(); }
        for &idx in &tx.locked_orecs {
            orecs()[idx].abort_release();
        }
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        inc_tx_abort_count();
        let ac = TM_ABORT_COUNT.load(Ordering::Relaxed);
        cm_backoff(ac);
        return false;
    }

    // Phase 5: release with new version
    let commit_ver = G_CLOCK.fetch_add(1, Ordering::Release) + 1;
    for &idx in &tx.locked_orecs {
        orecs()[idx].commit_release(commit_ver);
    }

    reset_tx_abort_count();
    true
}

// ── Init ────────────────────────────────────────────────
pub fn tm_init() {
    orecs();
    G_CLOCK.store(0, Ordering::Release);
    GREEDY_TS.store(0, Ordering::Release);
}

pub fn tm_exit() {}

pub fn tm_init_thread() {
    let sv = G_CLOCK.load(Ordering::Acquire);
    let ac = tx_abort_count();
    #[cfg(not(feature = "simulation"))]
    {
        let tid = THR_COUNTER.fetch_add(1, Ordering::Relaxed);
        TX.with(|tx| {
            *tx.borrow_mut() = Some(Box::new(TxState::new(sv, tid, ac)));
        });
    }
    #[cfg(feature = "simulation")]
    {
        let tid = runtime_core::current_sim_thread_id();
        let store = sim_state_store();
        let map = unsafe { &mut *store.get() };
        map.entry(tid).or_insert_with(|| SimThreadState {
            tx: Some(Box::new(TxState::new(sv, tid, ac))),
            succ_abort_count: ac,
        });
    }
}

pub fn tm_exit_thread() {
    #[cfg(not(feature = "simulation"))]
    TX.with(|tx| { *tx.borrow_mut() = None; });
    #[cfg(feature = "simulation")]
    {
        let tid = runtime_core::current_sim_thread_id();
        let store = sim_state_store();
        let map = unsafe { &mut *store.get() };
        if let Some(st) = map.get_mut(&tid) {
            st.tx = None;
        }
    }
}

pub fn tm_begin() {
    let sv = G_CLOCK.load(Ordering::Acquire);
    let ac = tx_abort_count();
    #[cfg(not(feature = "simulation"))]
    {
        let tid = THR_COUNTER.fetch_add(1, Ordering::Relaxed);
        TX.with(|tx| {
            *tx.borrow_mut() = Some(Box::new(TxState::new(sv, tid, ac)));
        });
    }
    #[cfg(feature = "simulation")]
    {
        let tid = runtime_core::current_sim_thread_id();
        let store = sim_state_store();
        let map = unsafe { &mut *store.get() };
        map.entry(tid).or_insert_with(|| SimThreadState {
            tx: None,
            succ_abort_count: 0,
        });
        if let Some(st) = map.get_mut(&tid) {
            st.tx = Some(Box::new(TxState::new(sv, tid, ac)));
        }
    }
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

// ── Simulation-only API ─────────────────────────────────
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
        let store = sim_state_store();
        let map = unsafe { &*store.get() };
        map.iter().map(|(k, st)| (*k, st.tx.clone())).collect()
    }

    pub fn restore_states(states: HashMap<u64, Option<Box<TxState>>>) {
        let store = sim_state_store();
        let map = unsafe { &mut *store.get() };
        map.clear();
        for (k, tx) in states {
            let ac = tx.as_ref().map_or(0, |t| t.succ_abort_count);
            map.insert(k, SimThreadState { tx, succ_abort_count: ac });
        }
    }

    pub fn reset() {
        let Some(tid) = runtime_core::try_current_sim_thread_id() else { return; };
        let store = sim_state_store();
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
        eprintln!("  STATS (SwissTM):");
        eprintln!("    Aborts={}", abt);
    }
}
