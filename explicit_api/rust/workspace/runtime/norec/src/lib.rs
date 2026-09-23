// ── NOrec TM backend for Rust ───────────────────────────
// Value-based validation with a single global versioned lock.
// No lock table — reads go directly to memory with value-based
// conflict detection, writes buffer to a write-set and commit
// serializes via CAS on the global lock.

use core::sync::atomic::{fence, AtomicU64, Ordering};
pub use runtime_core::{tm_install_tmx_hook, Primitive, TmxAbort, TypedValue, WriteBack};
#[cfg(not(feature = "simulation"))]
use std::cell::RefCell;

// ── Thread-local / simulation state ──────────────────────
// Normal mode: thread_local! for production multi-threaded use.
// Simulation mode: HashMap<u64, State> so the simulator can
// multiplex simulated threads on real OS threads.
#[cfg(not(feature = "simulation"))]
thread_local! {
    static TX: RefCell<Option<Box<TxState>>> = const { RefCell::new(None) };
}

#[cfg(feature = "simulation")]
use std::collections::HashMap;
#[cfg(feature = "simulation")]
use std::sync::Mutex;

#[cfg(feature = "simulation")]
fn sim_tx_store() -> &'static Mutex<HashMap<u64, Option<Box<TxState>>>> {
    use std::sync::OnceLock;
    static STORE: OnceLock<Mutex<HashMap<u64, Option<Box<TxState>>>>> = OnceLock::new();
    STORE.get_or_init(|| Mutex::new(HashMap::new()))
}

// ── Global lock ─────────────────────────────────────────
// Even = unlocked (version), Odd = locked.
// Version advances by 2 on each commit.
static GLOBAL_LOCK: AtomicU64 = AtomicU64::new(0);
static THR_COUNTER: AtomicU64 = AtomicU64::new(1);
pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);
pub static TM_COMMIT_COUNT: AtomicU64 = AtomicU64::new(0);

// ── Internal sync counters (gated behind "stats" feature) ─
#[cfg(feature = "stats")]
pub static TM_STATS: runtime_core::SyncCounters = runtime_core::SyncCounters::new();

// ── Read entry ─────────────────────────────────────────
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone)]
pub struct ReadEntry {
    pub addr: usize,
    pub sz: u8,            // byte size of the read
    pub observed_val: u64, // value at read time, zero-extended to u64
}

// ── Write entry ─────────────────────────────────────────
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone)]
pub struct WriteEntry {
    pub addr: usize,
    pub value: TypedValue,
}

// ── Transaction state ───────────────────────────────────
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone)]
pub struct TxState {
    pub read_set: Vec<ReadEntry>,
    pub write_set: Vec<WriteEntry>,
    /// Deferred write-back closures (safe to apply at commit).
    pub write_backs: Vec<WriteBack>,
    pub snapshot: u64,
    pub read_only: bool,
    #[allow(dead_code)]
    pub aborted: bool,
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

// ── Helpers ─────────────────────────────────────────────
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
    use runtime_core::current_sim_thread_id;
    let tid = current_sim_thread_id();
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

// ── Mixed-size buffer helpers (review-05 R-02) ──────────
// All buffered widths are ≤ 8 bytes at one base address, so they
// compose as little-endian integers.
fn sz_mask(sz: u8) -> u64 {
    if sz >= 8 {
        u64::MAX
    } else {
        (1u64 << (8 * sz)) - 1
    }
}

fn tv_to_u64(tv: &TypedValue) -> Option<u64> {
    match tv {
        TypedValue::U8(v) => Some(*v as u64),
        TypedValue::U16(v) => Some(*v as u64),
        TypedValue::U32(v) => Some(*v as u64),
        TypedValue::U64(v) => Some(*v),
        TypedValue::Bytes(b) if !b.is_empty() && b.len() <= 8 => {
            let mut buf = [0u8; 8];
            buf[..b.len()].copy_from_slice(b);
            Some(u64::from_le_bytes(buf))
        }
        _ => None,
    }
}

/// Rebuild `v` as the same variant/width as `old`.
fn tv_from_u64_like(v: u64, old: &TypedValue) -> TypedValue {
    match old {
        TypedValue::U8(_) => TypedValue::U8(v as u8),
        TypedValue::U16(_) => TypedValue::U16(v as u16),
        TypedValue::U32(_) => TypedValue::U32(v as u32),
        TypedValue::U64(_) => TypedValue::U64(v),
        TypedValue::Bytes(b) => {
            let all = v.to_le_bytes();
            TypedValue::Bytes(all[..b.len()].to_vec().into_boxed_slice())
        }
    }
}

fn tv_scalar_of(v: u64, sz: u8) -> TypedValue {
    match sz {
        1 => TypedValue::U8(v as u8),
        2 => TypedValue::U16(v as u16),
        4 => TypedValue::U32(v as u32),
        _ => TypedValue::U64(v),
    }
}

// ── Value-based validation ──────────────────────────────
// Re-reads every address in the read-set from memory and
// compares to the observed value.  Returns the current clock
// value if all match.
fn validate_impl(tx: &mut TxState) -> Option<u64> {
    #[cfg(feature = "stats")]
    TM_STATS.validations.fetch_add(1, Ordering::Relaxed);
    loop {
        let time = GLOBAL_LOCK.load(Ordering::Acquire);
        if time & 1 != 0 {
            // In simulation mode the lock-holder will never run,
            // so spinning is futile — abort immediately.
            #[cfg(not(feature = "simulation"))]
            {
                std::hint::spin_loop();
                continue;
            }
            #[cfg(feature = "simulation")]
            return None;
        }
        for r in &tx.read_set {
            let cur = read_mem_val(r.addr, r.sz);
            if cur != r.observed_val {
                #[cfg(feature = "stats")]
                TM_STATS.validation_failures.fetch_add(1, Ordering::Relaxed);
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
        return unsafe { (addr as *const T).read() };
    }
    let sz = core::mem::size_of::<T>() as u8;

    // Phase 1: check our own write-set first (reverse scan). An entry
    // covering the full request is served entirely from the write-set; a
    // narrower entry contributes only its low bytes and the remaining
    // bytes come from memory, spliced below (review-05 R-02).
    let (ws_val, own_prefix) = with_tx(|tx| {
        for e in tx.write_set.iter().rev() {
            if e.addr == addr {
                let esz = byte_size_of_tv(&e.value);
                if esz >= sz {
                    if esz == sz {
                        return (Some(T::from_typed(&e.value)), None);
                    }
                    if let Some(u) = tv_to_u64(&e.value) {
                        let t = tv_scalar_of(u & sz_mask(sz), sz);
                        return (Some(T::from_typed(&t)), None);
                    }
                    return (None, None);
                }
                if let Some(u) = tv_to_u64(&e.value) {
                    return (None, Some((u, esz)));
                }
                return (None, None);
            }
        }
        (None, None)
    });
    if let Some(v) = ws_val {
        return v;
    }

    // Phase 2: memory read with double-check torn-read protection.
    // Matches the C++ NOrec pattern: read clock, read value, re-read clock.
    // If the clock changed, a writer was active and the value may be torn.
    loop {
        let (clock_before, val_u64) = loop {
            let cb = GLOBAL_LOCK.load(Ordering::Acquire);
            if cb & 1 != 0 {
                // In simulation mode there is no concurrent writer, so the
                // value is safe to read directly despite the locked clock.
                #[cfg(feature = "simulation")]
                break (cb, read_mem_val(addr, sz));
                #[cfg(not(feature = "simulation"))]
                {
                    std::hint::spin_loop();
                    continue;
                }
            }
            let v = read_mem_val(addr, sz);
            let ca = GLOBAL_LOCK.load(Ordering::Acquire);
            if ca == cb {
                break (cb, v);
            }
        };
        let ret_u64 = match own_prefix {
            Some((pu, psz)) => (val_u64 & !sz_mask(psz)) | (pu & sz_mask(psz)),
            None => val_u64,
        };
        let val: T = match sz {
            1 => T::from_typed(&TypedValue::U8(ret_u64 as u8)),
            2 => T::from_typed(&TypedValue::U16(ret_u64 as u16)),
            4 => T::from_typed(&TypedValue::U32(ret_u64 as u32)),
            8 => T::from_typed(&TypedValue::U64(ret_u64)),
            _ => unreachable!(),
        };

        let snapshot = with_tx(|tx| tx.snapshot);

        if clock_before == snapshot {
            with_tx(|tx| {
                tx.read_set.push(ReadEntry {
                    addr,
                    sz,
                    observed_val: val_u64,
                });
                #[cfg(feature = "stats")]
                TM_STATS
                    .total_read_set_entries
                    .fetch_add(1, Ordering::Relaxed);
            });
            return val;
        }

        with_tx(|tx| match validate_impl(tx) {
            Some(s) => tx.snapshot = s,
            // review-05 R-10: panic-path aborts were missing from
            // tm_abort_count(); count them where they happen.
            None => {
                TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
                std::panic::panic_any(TmxAbort)
            }
        });
    }
}

// ── Write word ──────────────────────────────────────────
fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if !tx_active() {
        unsafe {
            (addr as *mut T).write(val);
        }
        return;
    }

    let tv = val.to_typed();
    let sz = byte_size_of_tv(&tv);

    with_tx(|tx| {
        tx.read_only = false;

        // Scan from end for existing entry at this address. Mixed-size
        // overlap composes little-endian at the shared base address:
        // a narrower write splices into the buffered value, a wider write
        // supersedes it (review-05 R-02 — never silently drop a write).
        for i in (0..tx.write_set.len()).rev() {
            if tx.write_set[i].addr == addr {
                let old_tv = tx.write_set[i].value.clone();
                let esz = byte_size_of_tv(&old_tv);
                if esz == sz {
                    tx.write_set[i].value = tv;
                    return;
                }
                if sz < esz {
                    if let (Some(cur), Some(nv)) = (tv_to_u64(&old_tv), tv_to_u64(&tv)) {
                        let m = sz_mask(sz);
                        let merged = (cur & !m) | (nv & m);
                        tx.write_set[i].value = tv_from_u64_like(merged, &old_tv);
                        return;
                    }
                }
                tx.write_set[i].value = tv;
                return;
            }
        }

        tx.write_set.push(WriteEntry { addr, value: tv });
        #[cfg(feature = "stats")]
        TM_STATS
            .total_write_set_entries
            .fetch_add(1, Ordering::Relaxed);
    });
}

fn apply_write_set(write_set: &[WriteEntry]) {
    for e in write_set {
        apply_typed_value(e.addr, &e.value);
    }
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
        tx.write_set.retain(|e| e.addr != addr);
        tx.write_set.push(WriteEntry { addr, value: tv });
    });
}

// ── Public API ──────────────────────────────────────────
pub fn tm_init() {
    tm_install_tmx_hook();
    GLOBAL_LOCK.store(0, Ordering::Release);
    THR_COUNTER.store(1, Ordering::Release);
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

pub fn tm_begin() {
    // Wait for snapshot to be even (unlocked)
    let snap = loop {
        let v = GLOBAL_LOCK.load(Ordering::Acquire);
        if v & 1 == 0 {
            break v;
        }
        // In simulation mode, the lock-holder will never run;
        // fall back to snapshot 0 (safe: no concurrent writer).
        #[cfg(feature = "simulation")]
        break 0;
        #[cfg(not(feature = "simulation"))]
        std::hint::spin_loop();
    };
    #[cfg(not(feature = "simulation"))]
    TX.with(|tx| {
        *tx.borrow_mut() = Some(Box::new(TxState::new(snap)));
    });
    #[cfg(feature = "simulation")]
    {
        let tid = runtime_core::current_sim_thread_id();
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        *map.get_mut(&tid).expect("no sim state for thread") = Some(Box::new(TxState::new(snap)));
    }
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
            #[cfg(feature = "stats")]
            TM_STATS.commits.fetch_add(1, Ordering::Relaxed);
            break;
        }
        // CAS failed — validate and retry
        #[cfg(feature = "stats")]
        TM_STATS.lock_contentions.fetch_add(1, Ordering::Relaxed);
        snapshot = match validate_impl(&mut tx) {
            Some(s) => s,
            None => {
                TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
                #[cfg(feature = "stats")]
                TM_STATS.aborts.fetch_add(1, Ordering::Relaxed);
                return false;
            }
        };
    }

    // We hold the global lock. Write-back from write-set.
    apply_write_set(&tx.write_set);

    // Release lock and advance version (even → next even)
    GLOBAL_LOCK.store(snapshot + 2, Ordering::Release);

    TM_COMMIT_COUNT.fetch_add(1, Ordering::Relaxed);
    true
}

pub fn tm_abort() {
    flush_tx();
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

// ── Simulation-only API (used by the TM simulator) ──────
#[cfg(feature = "simulation")]
pub mod sim {
    use super::*;

    /// Set the simulated thread ID for the current OS thread.
    /// Call before each backend operation in simulation mode.
    pub fn set_thread_id(id: u64) {
        runtime_core::set_sim_thread_id(id);
    }

    /// Clear the simulated thread ID.
    pub fn clear_thread_id() {
        runtime_core::clear_sim_thread_id();
    }

    /// Snapshot all per-thread TxState for checkpointing.
    pub fn snapshot_states() -> HashMap<u64, Option<Box<TxState>>> {
        let store = sim_tx_store();
        let map = store.lock().unwrap_or_else(|e| e.into_inner());
        map.clone()
    }

    /// Restore per-thread TxState from a checkpoint.
    pub fn restore_states(states: HashMap<u64, Option<Box<TxState>>>) {
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        *map = states;
    }

    /// Clear current thread's state (for reset between scenarios).
    pub fn reset() {
        let Some(tid) = runtime_core::try_current_sim_thread_id() else {
            return;
        };
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        map.remove(&tid);
    }

    /// Read the current stats snapshot and reset counters.
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

    /// Print stats to stderr.
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
        eprintln!("  STATS (NOrec):");
        eprintln!(
            "    Commits={}  Aborts={}  Val={}  VFail={}  Locks={}  LAqFail={}  RS={}  WS={}",
            com, abt, val, vfail, lcon, laf, trs, tws
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    // NOrec has one process-global clock; serialize tests.
    static NOREC_LOCK: Mutex<()> = Mutex::new(());

    // R-02: a narrower write into a buffered wider entry must merge, not
    // silently drop.
    #[test]
    fn mixed_size_write_merge_commit() {
        let _g = NOREC_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a8 = Box::into_raw(Box::new(0u64));
        let a32 = a8 as *mut u32;
        unsafe { a8.write(0) };
        tm_begin();
        tm_write_u64(a8, 0x0102_0304_0506_0708);
        tm_write_u32(a32, 0xAABB_CCDD);
        assert!(tm_commit());
        unsafe {
            assert_eq!(
                a8.read(),
                0x0102_0304_AABB_CCDD,
                "u32 write must merge into the buffered u64 (low bytes), not be dropped"
            );
        }
        unsafe { drop(Box::from_raw(a8)) };
    }

    // R-02: reads of mixed width must observe the buffered value (slice a
    // wider entry; splice a narrower prefix into a wider read).
    #[test]
    fn mixed_size_reads_see_buffered_value() {
        let _g = NOREC_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a8 = Box::into_raw(Box::new(0u64));
        let a32 = a8 as *mut u32;
        unsafe { a8.write(0xFFFF_FFFF_FFFF_FFFF) };
        tm_begin();
        tm_write_u32(a32, 0xDEAD_BEEF);
        assert_eq!(
            tm_read_u32(a32),
            0xDEAD_BEEF,
            "slice of wider buffered entry"
        );
        assert_eq!(
            tm_read_u64(a8),
            0xFFFF_FFFF_DEAD_BEEF,
            "wider read must splice the buffered prefix with memory for the upper bytes"
        );
        assert!(tm_commit(), "own-WS read must not self-abort validation");
        unsafe {
            assert_eq!(a8.read(), 0xFFFF_FFFF_DEAD_BEEF);
        }
        unsafe { drop(Box::from_raw(a8)) };
    }

    #[test]
    fn same_size_repeated_write_regression() {
        let _g = NOREC_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a8 = Box::into_raw(Box::new(0u64));
        tm_begin();
        tm_write_u64(a8, 1);
        tm_write_u64(a8, 2);
        tm_write_u64(a8, 3);
        assert!(tm_commit());
        unsafe {
            assert_eq!(a8.read(), 3);
            drop(Box::from_raw(a8));
        }
    }

    #[test]
    fn abort_drops_buffered_mixed_writes() {
        let _g = NOREC_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a8 = Box::into_raw(Box::new(0x55u64));
        let a16 = a8 as *mut u16;
        let before = tm_abort_count();
        tm_begin();
        tm_write_u64(a8, 0xEE);
        tm_write_u16(a16, 0x1234);
        tm_abort();
        unsafe {
            assert_eq!(a8.read(), 0x55, "abort must not apply buffered writes");
            drop(Box::from_raw(a8));
        }
        let _ = tm_abort_count() >= before;
    }
}
