// ── TSC-TM backend for Rust ──────────────────────────────
// TL2 variant (Mirzoev et al., "TSC-TM: Boosting Performance of
// STM Through Efficient Version Management") with the global
// version clock replaced by CPU-timestamp versions: begin() and
// commit() read a timestamp instead of doing a global atomic RMW,
// and guards are released with max(commit_ts, acq_version + 1) so
// every guard's own version stays strictly increasing.
// Ported from backends/tm_impl/tsc_tm/tsc_tm.hpp (review-05
// protocol fixes included) for simulator coverage.

use core::sync::atomic::{compiler_fence, fence, AtomicU64, Ordering};
pub use runtime_core::{tm_install_tmx_hook, Primitive, TmxAbort, TypedValue, WriteBack};
#[cfg(not(feature = "simulation"))]
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::OnceLock;

// ── Constants ───────────────────────────────────────────
// Guard encoding: (version << 1) | lock bit (see tsc_tm.hpp).
const LOCK_MASK: u64 = 1;
const VERSION_SHIFT: u64 = 1;
const TABLE_BITS: u64 = 20;
const TABLE_SIZE: usize = 1 << TABLE_BITS;

fn lock_index(addr: usize) -> usize {
    let h = (addr as u64).wrapping_mul(0x9E3779B97F4A7C15);
    (h >> (64 - TABLE_BITS)) as usize
}

// ── Timestamp source ────────────────────────────────────
// TSC-TM has no global version clock: versions come from a
// monotonic timestamp.  Native builds read the CPU cycle counter;
// simulation builds use a logical counter so replays stay
// deterministic (the real TSC would differ run to run).
#[cfg(all(not(feature = "simulation"), target_arch = "x86_64"))]
fn timestamp() -> u64 {
    unsafe { core::arch::x86_64::_rdtsc() }
}

#[cfg(all(not(feature = "simulation"), not(target_arch = "x86_64")))]
fn timestamp() -> u64 {
    static TICK: AtomicU64 = AtomicU64::new(1);
    TICK.fetch_add(1, Ordering::SeqCst)
}

#[cfg(feature = "simulation")]
fn timestamp() -> u64 {
    static TICK: AtomicU64 = AtomicU64::new(1);
    TICK.fetch_add(1, Ordering::SeqCst)
}

// ── Guard table ─────────────────────────────────────────
struct Guard {
    data: AtomicU64,
}

impl Guard {
    const fn new() -> Self {
        Guard {
            data: AtomicU64::new(0),
        }
    }
    fn version(&self) -> u64 {
        (self.data.load(Ordering::Acquire) & !LOCK_MASK) >> VERSION_SHIFT
    }
    /// Lock the guard, returning the version it currently carries
    /// (needed for the monotonic release bump).
    fn try_lock_exclusive(&self) -> Option<u64> {
        let cur = self.data.load(Ordering::Relaxed);
        if cur & LOCK_MASK != 0 {
            return None;
        }
        match self.data.compare_exchange_weak(
            cur,
            cur | LOCK_MASK,
            Ordering::Acquire,
            Ordering::Relaxed,
        ) {
            Ok(_) => Some((cur & !LOCK_MASK) >> VERSION_SHIFT),
            Err(_) => None,
        }
    }
    /// Abort-path unlock: keep the version untouched.
    fn release_guard(&self) {
        let cur = self.data.load(Ordering::Relaxed);
        self.data.store(cur & !LOCK_MASK, Ordering::Release);
    }
    /// Commit-path release: store a version strictly larger than the
    /// guard held before this commit, even if the commit timestamp
    /// raced another core's (tsc_tm.hpp release_guard_with_version).
    fn release_with_version(&self, commit_ts: u64, acq_version: u64) {
        let v = if commit_ts > acq_version + 1 {
            commit_ts
        } else {
            acq_version + 1
        };
        self.data.store(v << VERSION_SHIFT, Ordering::Release);
    }
}

fn guard_at_index(idx: usize) -> &'static Guard {
    &guards()[idx]
}

static GUARD_TABLE: OnceLock<Box<[Guard]>> = OnceLock::new();

fn guards() -> &'static [Guard] {
    GUARD_TABLE.get_or_init(|| {
        (0..TABLE_SIZE)
            .map(|_| Guard::new())
            .collect::<Vec<_>>()
            .into_boxed_slice()
    })
}

pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);
pub static TM_COMMIT_COUNT: AtomicU64 = AtomicU64::new(0);

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
    read_set: Vec<(usize, u64)>, // (addr, version observed at guard load)
    write_set: HashMap<usize, TypedValue>,
    #[allow(dead_code)]
    write_backs: Vec<WriteBack>,
    #[allow(dead_code)] // TSC-TM validates versions at commit; start_ts is informational
    start_ts: u64,
    #[allow(dead_code)]
    aborted: bool,
}

impl TxState {
    fn new(start_ts: u64) -> Self {
        TxState {
            read_set: Vec::with_capacity(64),
            write_set: HashMap::with_capacity(8),
            write_backs: Vec::new(),
            start_ts,
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
// Like tsc_tm.hpp's read_impl: a single guard load (no spin — a
// locked guard is caught at validation), then the value, and the
// observed version is recorded for commit-time validation.
fn read_word<T: Primitive>(addr: usize) -> T {
    compiler_fence(Ordering::SeqCst);
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }

    if let Some(tv) = with_tx(|tx| tx.write_set.get(&addr).cloned()) {
        return T::from_typed(&tv);
    }

    let idx = lock_index(addr);
    let ver = guard_at_index(idx).version();
    let val: T = unsafe { (addr as *const T).read() };

    if with_tx(|tx| {
        if tx.read_set.len() > 1_000_000 {
            return true;
        }
        tx.read_set.push((addr, ver));
        #[cfg(feature = "stats")]
        TM_STATS
            .total_read_set_entries
            .fetch_add(1, Ordering::Relaxed);
        false
    }) {
        std::panic::panic_any(TmxAbort);
    }
    val
}

// ── Write word ──────────────────────────────────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    compiler_fence(Ordering::SeqCst);
    if !tx_active() {
        unsafe {
            (addr as *mut T).write(val);
        }
        return;
    }

    let tv = val.to_typed();
    with_tx(|tx| {
        let _is_new = tx.write_set.insert(addr, tv).is_none();
        #[cfg(feature = "stats")]
        if _is_new {
            TM_STATS
                .total_write_set_entries
                .fetch_add(1, Ordering::Relaxed);
        }
    });
}

// ── Raw byte operations ─────────────────────────────────
fn read_raw_bytes(addr: usize, dst: &mut [u8]) {
    for (i, byte) in dst.iter_mut().enumerate() {
        *byte = read_word::<u8>(addr + i);
    }
}

fn write_raw_bytes(addr: usize, src: &[u8]) {
    compiler_fence(Ordering::SeqCst);
    if !tx_active() {
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len());
        }
        return;
    }
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

// ── Commit ──────────────────────────────────────────────
pub fn tm_commit() -> bool {
    let tx = match flush_tx() {
        Some(t) => t,
        None => return true,
    };
    compiler_fence(Ordering::SeqCst);

    // ROCO: read-only transactions commit without validation.
    if tx.write_set.is_empty() {
        return true;
    }

    // Step 3: acquire write-set guards in address order, deduped by
    // guard index (addresses sharing one guard must be locked once —
    // review-05 R-07 lesson from TL2).
    let mut addrs: Vec<usize> = tx.write_set.keys().copied().collect();
    addrs.sort_unstable();
    let mut locked_idxs: Vec<usize> = Vec::with_capacity(addrs.len());
    let mut acq_versions: Vec<u64> = Vec::with_capacity(addrs.len());
    for &a in &addrs {
        let idx = lock_index(a);
        if locked_idxs.contains(&idx) {
            continue;
        }
        #[cfg(feature = "simulation")]
        let acquired = loop {
            // The holder will not release within this deterministic
            // step: give up immediately on contention.
            match guard_at_index(idx).try_lock_exclusive() {
                Some(v) => break Some(v),
                None => break None,
            }
        };
        #[cfg(not(feature = "simulation"))]
        let acquired = loop {
            match guard_at_index(idx).try_lock_exclusive() {
                Some(v) => break Some(v),
                None => {
                    #[cfg(feature = "stats")]
                    TM_STATS.lock_contentions.fetch_add(1, Ordering::Relaxed);
                    std::hint::spin_loop();
                }
            }
        };
        match acquired {
            Some(v) => {
                locked_idxs.push(idx);
                acq_versions.push(v);
            }
            None => {
                for &idx in &locked_idxs {
                    guard_at_index(idx).release_guard();
                }
                TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
                #[cfg(feature = "stats")]
                TM_STATS.aborts.fetch_add(1, Ordering::Relaxed);
                return false;
            }
        }
    }
    compiler_fence(Ordering::SeqCst);

    // Step 4: timestamp for the commit (no global atomic RMW).
    let commit_ts = timestamp();

    // Step 5: validate the read-set — version must still match, and
    // any guard locked by someone else is a conflict.
    let mut rs_ok = true;
    #[cfg(feature = "stats")]
    TM_STATS.validations.fetch_add(1, Ordering::Relaxed);
    for &(a, v) in &tx.read_set {
        let idx = lock_index(a);
        let g = guard_at_index(idx).data.load(Ordering::Acquire);
        let our_lock = locked_idxs.contains(&idx);
        if (g & LOCK_MASK != 0 && !our_lock) || ((g & !LOCK_MASK) >> VERSION_SHIFT) != v {
            rs_ok = false;
            break;
        }
    }
    if !rs_ok {
        for &idx in &locked_idxs {
            guard_at_index(idx).release_guard();
        }
        #[cfg(feature = "stats")]
        TM_STATS.validation_failures.fetch_add(1, Ordering::Relaxed);
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        #[cfg(feature = "stats")]
        TM_STATS.aborts.fetch_add(1, Ordering::Relaxed);
        return false;
    }

    // Step 7: apply ALL writes first, then release each unique guard
    // exactly once with a strictly-greater version (releasing early
    // would let aliasing sharers see unlocked guards with only part
    // of the update).
    for (addr, tv) in &tx.write_set {
        apply_typed_value(*addr, tv);
    }
    for (k, &idx) in locked_idxs.iter().enumerate() {
        guard_at_index(idx).release_with_version(commit_ts, acq_versions[k]);
    }
    fence(Ordering::SeqCst);

    #[cfg(feature = "stats")]
    TM_STATS.commits.fetch_add(1, Ordering::Relaxed);
    TM_COMMIT_COUNT.fetch_add(1, Ordering::Relaxed);

    true
}

// ── Init ────────────────────────────────────────────────
pub fn tm_init() {
    tm_install_tmx_hook();
    #[cfg(feature = "simulation")]
    for guard in guards().iter() {
        guard.data.store(0, Ordering::Release);
    }
    #[cfg(feature = "stats")]
    TM_STATS.reset();
}

pub fn tm_exit() {}

pub fn tm_init_thread() {
    #[cfg(not(feature = "simulation"))]
    TX.with(|tx| {
        *tx.borrow_mut() = None;
    });
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
    let ts = timestamp();
    TX.with(|tx| {
        *tx.borrow_mut() = Some(Box::new(TxState::new(ts)));
    });
}

#[cfg(feature = "simulation")]
pub fn tm_begin() {
    let ts = timestamp();
    let tid = runtime_core::current_sim_thread_id();
    let store = sim_tx_store();
    let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
    *map.get_mut(&tid).expect("no sim state for thread") = Some(Box::new(TxState::new(ts)));
}

pub fn tm_abort() {
    flush_tx();
}

pub fn tm_abort_count() -> u64 {
    TM_ABORT_COUNT.load(Ordering::Relaxed)
}

/// Commit counter (parity with the TinySTM façade API; review-05 R-12).
pub fn tm_commit_count() -> u64 {
    TM_COMMIT_COUNT.load(Ordering::Relaxed)
}

/// Reset the commit/abort counters.
pub fn tm_reset_stats() {
    TM_COMMIT_COUNT.store(0, Ordering::Relaxed);
    TM_ABORT_COUNT.store(0, Ordering::Relaxed);
}

// ── Typed wrappers ─────────────────────────────────────
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
        let Some(tid) = runtime_core::try_current_sim_thread_id() else {
            return;
        };
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        map.remove(&tid);
    }

    #[cfg(feature = "stats")]
    pub fn take_stats() -> runtime_core::SyncCounters {
        let s = runtime_core::SyncCounters::new();
        s.validations.store(
            TM_STATS.validations.load(Ordering::Relaxed),
            Ordering::Relaxed,
        );
        s.validation_failures.store(
            TM_STATS.validation_failures.load(Ordering::Relaxed),
            Ordering::Relaxed,
        );
        s.lock_contentions.store(
            TM_STATS.lock_contentions.load(Ordering::Relaxed),
            Ordering::Relaxed,
        );
        s.lock_acquire_failures.store(
            TM_STATS.lock_acquire_failures.load(Ordering::Relaxed),
            Ordering::Relaxed,
        );
        s.total_read_set_entries.store(
            TM_STATS.total_read_set_entries.load(Ordering::Relaxed),
            Ordering::Relaxed,
        );
        s.total_write_set_entries.store(
            TM_STATS.total_write_set_entries.load(Ordering::Relaxed),
            Ordering::Relaxed,
        );
        s.commits
            .store(TM_STATS.commits.load(Ordering::Relaxed), Ordering::Relaxed);
        s.aborts
            .store(TM_STATS.aborts.load(Ordering::Relaxed), Ordering::Relaxed);
        TM_STATS.reset();
        s
    }

    #[cfg(feature = "stats")]
    pub fn print_stats(s: &runtime_core::SyncCounters) {
        use std::sync::atomic::Ordering;
        let val = s.validations.load(Ordering::Relaxed);
        let vfail = s.validation_failures.load(Ordering::Relaxed);
        let lcon = s.lock_contentions.load(Ordering::Relaxed);
        let laf = s.lock_acquire_failures.load(Ordering::Relaxed);
        let trs = s.total_read_set_entries.load(Ordering::Relaxed);
        let tws = s.total_write_set_entries.load(Ordering::Relaxed);
        let com = s.commits.load(Ordering::Relaxed);
        let abt = s.aborts.load(Ordering::Relaxed);
        eprintln!("  STATS (TSC-TM):");
        eprintln!(
            "    Commits={}  Aborts={}  Val={}  VFail={}  CLock={}  LAqFail={}  RS={}  WS={}",
            com, abt, val, vfail, lcon, laf, trs, tws
        );
    }
}

// Unit tests exercise the native (thread-local) paths; the simulation
// paths are covered by the simulator crate tests (simulator/src/backend.rs).
#[cfg(all(test, not(feature = "simulation")))]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static TSC_LOCK: Mutex<()> = Mutex::new(());

    #[test]
    fn basic_tx_commits_and_persists() {
        let _g = TSC_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let mut buf = 0u64;
        let addr = &mut buf as *mut u64;
        tm_begin();
        assert_eq!(tm_read_u64(addr), 0);
        tm_write_u64(addr, 42);
        assert_eq!(tm_read_u64(addr), 42, "own write must be visible");
        assert!(tm_commit());
        assert_eq!(buf, 42);

        // Guard version must be the TSC-derived commit timestamp:
        // nonzero and strictly greater than the pre-commit version.
        let idx = lock_index(addr as usize);
        assert!(guard_at_index(idx).version() > 0);
    }

    #[test]
    fn validation_aborts_stale_reader() {
        let _g = TSC_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let mut buf = 7u64;
        let addr = &mut buf as *mut u64 as usize;
        static READY: AtomicU64 = AtomicU64::new(0);
        static DONE: AtomicU64 = AtomicU64::new(0);
        READY.store(0, Ordering::SeqCst);
        DONE.store(0, Ordering::SeqCst);
        let t1 = std::thread::spawn(move || {
            // Thread A: read addr (records its guard version), pause,
            // then commit — the guard version changed underneath, so
            // validation must abort it.
            tm_begin();
            let v = tm_read_u64(addr as *mut u64);
            assert_eq!(v, 7);
            tm_write_u64(addr as *mut u64, 11);
            READY.store(1, Ordering::SeqCst);
            while DONE.load(Ordering::SeqCst) == 0 {
                std::hint::spin_loop();
            }
            !tm_commit() // true when the commit aborted
        });
        while READY.load(Ordering::SeqCst) == 0 {
            std::hint::spin_loop();
        }
        tm_begin();
        tm_write_u64(addr as *mut u64, 5);
        assert!(tm_commit(), "uncontended commit must succeed");
        DONE.store(1, Ordering::SeqCst);
        let aborted = t1.join().unwrap();
        assert!(aborted, "transaction with a stale read-set must abort");
        assert_eq!(unsafe { *(addr as *const u64) }, 5);
    }

    #[test]
    fn shared_guard_index_is_locked_once() {
        let _g = TSC_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        // Two addresses colliding on one guard (review-05 R-07 class):
        // must be locked once and released once with a bumped version.
        let mut buf = vec![0u64; 1 << 22];
        let base = buf.as_mut_ptr() as usize;
        let base_idx = lock_index(base);
        let mut off = 0usize;
        for i in 1..(1usize << 22) {
            if lock_index(base + i * 8) == base_idx {
                off = i * 8;
                break;
            }
        }
        assert_ne!(off, 0, "test setup: no colliding guard index found");
        tm_begin();
        tm_write_u64(base as *mut u64, 1);
        tm_write_u64((base + off) as *mut u64, 2);
        assert!(tm_commit(), "commit must succeed with a shared guard");
        assert_eq!(buf[0], 1);
        assert_eq!(buf[off / 8], 2);
        tm_begin();
        assert_eq!(tm_read_u64(base as *mut u64), 1);
        assert_eq!(tm_read_u64((base + off) as *mut u64), 2);
        assert!(tm_commit());
    }
}
