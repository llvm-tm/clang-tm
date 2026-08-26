// ── MVLog TM backend for Rust ───────────────────────────
// Multi-version commit-log STM.  Each transaction negotiates its
// commit position (a log slot) with one atomic fetch_add on a global
// counter at begin, logs its write-set in a global ever-growing commit
// log, and tracks the newest committed writer per address in an index
// table.  A no-false-negative Bloom filter over recently-written
// addresses lets the read fast path be a plain memory load.
//
// Mirrors backends/tm_impl/mvlog/MVLog.hpp (C++).

use core::sync::atomic::{fence, AtomicPtr, AtomicU64, AtomicU8, AtomicUsize, Ordering};
#[cfg(not(feature = "simulation"))]
use std::cell::RefCell;
pub use runtime_core::{tm_install_tmx_hook, Primitive, TmxAbort, TypedValue, WriteBack};

// ── Global geometry (matches C++ MVLog.hpp) ─────────────
const KLOG_BITS: usize = 17; // 2^17 ring entries
const KLOG_SLOTS: usize = 1 << KLOG_BITS;
const KLOG_MASK: usize = KLOG_SLOTS - 1;
const KMAX_INLINE_WS: usize = 8; // inline write entries per log entry
const KRECLAIM_THRESHOLD: u64 = 1 << 14; // reclaim when window exceeds
const KINDEX_BITS: usize = 20;
const KINDEX_SLOTS: usize = 1 << KINDEX_BITS;

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum LogState {
    Free = 0,
    Progress = 1,
    Committed = 2,
    Aborted = 3,
}

impl From<u32> for LogState {
    fn from(v: u32) -> Self {
        match v {
            0 => LogState::Free,
            1 => LogState::Progress,
            2 => LogState::Committed,
            3 => LogState::Aborted,
            _ => LogState::Free,
        }
    }
}

// ── Thread-local / simulation state ─────────────────────
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

// ── Globals ─────────────────────────────────────────────
static G_NEXT: AtomicU64 = AtomicU64::new(1);
static G_WM: AtomicU64 = AtomicU64::new(0);
static G_COMMIT_LOCK: AtomicU64 = AtomicU64::new(0);
pub static TM_ABORT_COUNT: AtomicU64 = AtomicU64::new(0);

// ── Internal sync counters (gated behind "stats" feature) ─
#[cfg(feature = "stats")]
pub static TM_STATS: runtime_core::SyncCounters = runtime_core::SyncCounters::new();

// Log entry: state/tag are atomic; the write-set is stored inline
// (KMAX_INLINE_WS fixed entries) with an overflow boxed vector for
// larger write-sets.  Writes to a committed entry are immutable.
// The write-set fields are written before the state release-store and
// read after the state acquire-load, so Relaxed ordering on them is
// safe (the state load/store provides the publish synchronization).
struct LogEntry {
    state: AtomicU64, // LogState as u64
    tag: AtomicU64,
    ws_count: AtomicUsize,
    ws_addr: [AtomicUsize; KMAX_INLINE_WS],
    ws_type: [AtomicU8; KMAX_INLINE_WS], // ValueType tag as u8
    ws_val: [AtomicU64; KMAX_INLINE_WS],
    // Overflow write-set: raw pointer to a Box<Vec<(usize,u8,u64)>>, or null.
    // Leaked (never freed) once published — matches C++ MVLog, which also
    // never frees overflow arrays.
    overflow: AtomicPtr<u8>,
}

const LOG_ENTRY_FREE: LogEntry = LogEntry {
    state: AtomicU64::new(0),
    tag: AtomicU64::new(0),
    ws_count: AtomicUsize::new(0),
    ws_addr: [const { AtomicUsize::new(0) }; KMAX_INLINE_WS],
    ws_type: [const { AtomicU8::new(0) }; KMAX_INLINE_WS],
    ws_val: [const { AtomicU64::new(0) }; KMAX_INLINE_WS],
    overflow: AtomicPtr::new(core::ptr::null_mut()),
};

static G_LOG: [LogEntry; KLOG_SLOTS] = [LOG_ENTRY_FREE; KLOG_SLOTS];

// Index table: open addressing, addr -> newest committed writer slot.
struct IndexBucket {
    addr: AtomicU64,
    slot: AtomicU64,
}

const INDEX_BUCKET_FREE: IndexBucket = IndexBucket {
    addr: AtomicU64::new(0),
    slot: AtomicU64::new(u64::MAX), // -1 as u64 sentinel
};

static G_INDEX: [IndexBucket; KINDEX_SLOTS] = [INDEX_BUCKET_FREE; KINDEX_SLOTS];

// ── Bloom filter over dirty (recently-written) addresses ─
const BLOOM_WORDS: usize = 64;
struct BloomFilter {
    words: [AtomicU64; BLOOM_WORDS],
}

const BLOOM_FREE: BloomFilter = BloomFilter {
    words: [const { AtomicU64::new(0) }; BLOOM_WORDS],
};

static DIRTY: BloomFilter = BLOOM_FREE;

fn bloom_hash(addr: u64) -> (u64, u64) {
    let h = addr.wrapping_mul(0x9E3779B97F4A7C15u64).rotate_left(7);
    let h2 = h.rotate_left(17) ^ (h >> 3);
    (h, h2)
}

fn bloom_insert(addr: u64) {
    let (h1, h2) = bloom_hash(addr);
    DIRTY.words[(h1 as usize) % BLOOM_WORDS].store(1, Ordering::Relaxed);
    DIRTY.words[(h2 as usize) % BLOOM_WORDS].store(1, Ordering::Relaxed);
}

fn bloom_contains(addr: u64) -> bool {
    let (h1, h2) = bloom_hash(addr);
    DIRTY.words[(h1 as usize) % BLOOM_WORDS].load(Ordering::Relaxed) != 0
        && DIRTY.words[(h2 as usize) % BLOOM_WORDS].load(Ordering::Relaxed) != 0
}

fn bloom_clear() {
    for w in &DIRTY.words {
        w.store(0, Ordering::Relaxed);
    }
}

// ── Read / write entries ────────────────────────────────
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone)]
pub struct ReadEntry {
    pub addr: usize,
    pub sz: u8,            // byte size of the read
    pub observed_val: u64, // value at read time, zero-extended to u64
}

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
    pub slot: u64,
    pub active: bool,
    pub read_set: Vec<ReadEntry>,
    pub write_set: Vec<WriteEntry>,
    /// Deferred write-back closures (safe to apply at commit).
    pub write_backs: Vec<WriteBack>,
    pub read_only: bool,
    #[allow(dead_code)]
    pub aborted: bool,
    pub abort_count: u64,
}

impl TxState {
    fn new() -> Self {
        TxState {
            slot: 0,
            active: false,
            read_set: Vec::with_capacity(64),
            write_set: Vec::with_capacity(8),
            write_backs: Vec::new(),
            read_only: true,
            aborted: false,
            abort_count: 0,
        }
    }

    fn reset(&mut self) {
        self.active = false;
        self.read_set.clear();
        self.write_set.clear();
        self.write_backs.clear();
        self.read_only = true;
        self.aborted = false;
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

fn write_mem_val(addr: usize, val: u64, sz: u8) {
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

fn byte_size_of_tv(tv: &TypedValue) -> u8 {
    match tv {
        TypedValue::U8(_) => 1,
        TypedValue::U16(_) => 2,
        TypedValue::U32(_) => 4,
        TypedValue::U64(_) => 8,
        TypedValue::Bytes(b) => b.len() as u8,
    }
}

// ValueType tag used in the log write-set (0..5).
fn value_type_tag(tv: &TypedValue) -> u8 {
    match tv {
        TypedValue::U8(_) => 0,
        TypedValue::U16(_) => 1,
        TypedValue::U32(_) => 2,
        TypedValue::U64(_) => 3,
        TypedValue::Bytes(_) => 4,
    }
}

fn tv_to_u64(tv: &TypedValue) -> u64 {
    match tv {
        TypedValue::U8(v) => *v as u64,
        TypedValue::U16(v) => *v as u64,
        TypedValue::U32(v) => *v as u64,
        TypedValue::U64(v) => *v,
        TypedValue::Bytes(b) => {
            let mut out = [0u8; 8];
            for (i, byte) in b.iter().take(8).enumerate() {
                out[i] = *byte;
            }
            u64::from_le_bytes(out)
        }
    }
}

// ── Index helpers ───────────────────────────────────────
fn hash_addr(addr: u64) -> u64 {
    addr.wrapping_mul(0x9E3779B97F4A7C15u64).rotate_left(11)
}

// Newest committed writer slot for addr, or u64::MAX if absent.
fn index_lookup(addr: u64) -> u64 {
    let h = hash_addr(addr) as usize & (KINDEX_SLOTS - 1);
    for i in 0..KINDEX_SLOTS {
        let b = &G_INDEX[(h + i) & (KINDEX_SLOTS - 1)];
        let a = b.addr.load(Ordering::Acquire);
        if a == addr {
            return b.slot.load(Ordering::Acquire);
        }
        if a == 0 {
            return u64::MAX;
        }
    }
    u64::MAX
}

// Insert or update.  Publishers serialize in slot order under the
// commit lock, so this is single-writer per bucket chain.
fn index_insert(addr: u64, slot: u64) {
    let h = hash_addr(addr) as usize & (KINDEX_SLOTS - 1);
    for i in 0..KINDEX_SLOTS {
        let b = &G_INDEX[(h + i) & (KINDEX_SLOTS - 1)];
        let a = b.addr.load(Ordering::Relaxed);
        if a == addr {
            b.slot.store(slot, Ordering::Release);
            return;
        }
        if a == 0 {
            b.addr.store(addr, Ordering::Relaxed);
            b.slot.store(slot, Ordering::Release);
            return;
        }
    }
    unreachable!("index full");
}

// ── Resolve: newest committed value for addr ────────────
// Reads the newest committed writer's log entry via the index.  Retries
// while the entry is recycled under us; terminates because recycling
// implies the slot was folded and the `t < w` branch then reads memory.
fn resolve(addr: usize, sz: u8) -> u64 {
    loop {
        let w = G_WM.load(Ordering::Acquire);
        let slot = index_lookup(addr as u64);
        if slot == u64::MAX {
            return read_mem_val(addr, sz);
        }
        if slot < w {
            return read_mem_val(addr, sz);
        }
        let e = &G_LOG[slot as usize & KLOG_MASK];
        let tag = e.tag.load(Ordering::Acquire);
        if tag != slot {
            continue; // recycled — re-resolve
        }
        let st: LogState = (e.state.load(Ordering::Acquire) as u32 & 0xFF).into();
        if st != LogState::Committed {
            continue; // not yet published — re-resolve
        }
        let n = e.ws_count.load(Ordering::Relaxed);
        let inline = n.min(KMAX_INLINE_WS);
        for i in 0..inline {
            let eaddr = e.ws_addr[i].load(Ordering::Relaxed);
            if eaddr == addr {
                let t = e.ws_type[i].load(Ordering::Relaxed);
                let v = e.ws_val[i].load(Ordering::Relaxed);
                // Type-interchange: the entry covers the read if it is
                // wider or the same width; extract the low bytes.
                return extract_value(v, t, sz);
            }
        }
        if n > KMAX_INLINE_WS {
            let p = e.overflow.load(Ordering::Relaxed);
            if !p.is_null() {
                // SAFETY: the overflow box is published before the state
                // release-store and never freed, so once we observe
                // COMMITTED the pointer is valid and immutable.
                let ov = unsafe { &*(p as *const Vec<(usize, u8, u64)>) };
                for &(oaddr, t, v) in ov.iter() {
                    if oaddr == addr {
                        return extract_value(v, t, sz);
                    }
                }
            }
        }
        // Address written by this slot but not covering the exact read:
        // fall through to memory (read-own-write handled at tx level).
        return read_mem_val(addr, sz);
    }
}

fn extract_value(v: u64, _t: u8, sz: u8) -> u64 {
    match sz {
        1 => v & 0xFF,
        2 => v & 0xFFFF,
        4 => v & 0xFFFFFFFF,
        8 => v,
        _ => v,
    }
}

// ── Transaction begin ───────────────────────────────────
// If a previous transaction of this thread is still in flight (aborted
// via panic and not yet re-claimed), resolve its slot as ABORTED first.
fn begin_impl(tx: &mut TxState) {
    if tx.active {
        G_LOG[tx.slot as usize & KLOG_MASK]
            .state
            .store(LogState::Aborted as u64, Ordering::Release);
    }

    let slot = G_NEXT.fetch_add(1, Ordering::AcqRel);
    let e = &G_LOG[slot as usize & KLOG_MASK];
    e.tag.store(slot, Ordering::Relaxed);
    e.state.store(LogState::Progress as u64, Ordering::Release);

    tx.slot = slot;
    tx.active = true;
    tx.aborted = false;
    tx.abort_count = 0;
    tx.read_set.clear();
    tx.write_set.clear();
    tx.write_backs.clear();
    tx.read_only = true;
}

// ── Abort ───────────────────────────────────────────────
fn abort_impl(tx: &mut TxState) {
    tx.abort_count += 1;
    TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
    tx.read_set.clear();
    tx.write_set.clear();
    // The slot stays PROGRESS; the retry's begin() resolves it as
    // ABORTED.  Signal the retry loop via panic.
    std::panic::panic_any(TmxAbort);
}

// ── Commit ──────────────────────────────────────────────
// Phase 1: wait for every slot in the live window [g_wm, slot) to
// resolve (enforcing commit order == slot-claim order).
// Phase 2: value-validate the read-set against the final index.
// Phase 3: publish the write-set, update the index, reclaim if due.
fn commit_impl(tx: &mut TxState) {
    debug_assert!(tx.active, "tx not active");
    let slot = tx.slot;

    // Phase 1 — wait for predecessors in the live window.
    let w = G_WM.load(Ordering::Acquire);
    for s in w..slot {
        let e = &G_LOG[s as usize & KLOG_MASK];
        loop {
            let st: LogState = (e.state.load(Ordering::Acquire) as u32 & 0xFF).into();
            if st != LogState::Progress {
                break;
            }
            let tag = e.tag.load(Ordering::Acquire);
            if tag != s {
                break; // recycled → resolved & folded
            }
            #[cfg(not(feature = "simulation"))]
            std::hint::spin_loop();
            #[cfg(feature = "simulation")]
            {
                if s == slot.saturating_sub(1) {
                    break;
                }
            }
        }
    }

    // Commit lock: makes validate → publish atomic w.r.t. other commits.
    // Acquired AFTER the predecessor wait so we never hold the lock while
    // waiting on a slot whose resolution requires another commit.
    loop {
        if G_COMMIT_LOCK.compare_exchange_weak(0, 1, Ordering::Acquire, Ordering::Relaxed).is_ok()
        {
            break;
        }
        #[cfg(not(feature = "simulation"))]
        std::hint::spin_loop();
        #[cfg(feature = "simulation")]
        break; // no concurrent commit in simulation single-thread
    }

    // Phase 2 — validate the read-set against the (now frozen) commit state.
    // Resolve all reads first (collecting conflicts) so the read_set borrow
    // ends before abort_impl takes &mut tx.
    let conflict = tx
        .read_set
        .iter()
        .any(|r| resolve(r.addr, r.sz) != r.observed_val);
    if conflict {
        G_COMMIT_LOCK.store(0, Ordering::Release);
        abort_impl(tx);
    }
    // Phase 3 — publish.
    let e = &G_LOG[slot as usize & KLOG_MASK];
    e.ws_count.store(0, Ordering::Relaxed);
    if tx.write_set.is_empty() {
        // Read-only: mark resolved, no index/dirty updates.
        e.state.store(LogState::Committed as u64, Ordering::Release);
        G_COMMIT_LOCK.store(0, Ordering::Release);
        tx.reset();
        return;
    }

    let n = tx.write_set.len().min(KMAX_INLINE_WS);
    for i in 0..n {
        let we = &tx.write_set[i];
        e.ws_addr[i].store(we.addr, Ordering::Relaxed);
        e.ws_type[i].store(value_type_tag(&we.value), Ordering::Relaxed);
        e.ws_val[i].store(tv_to_u64(&we.value), Ordering::Relaxed);
    }
    if tx.write_set.len() > KMAX_INLINE_WS {
        let mut ov = Vec::with_capacity(tx.write_set.len());
        for we in &tx.write_set {
            ov.push((we.addr, value_type_tag(&we.value), tv_to_u64(&we.value)));
        }
        let boxed = Box::new(ov);
        e.overflow.store(Box::into_raw(boxed).cast(), Ordering::Relaxed);
    }
    e.ws_count.store(tx.write_set.len(), Ordering::Relaxed);

    // Write-through to memory: peek()-style direct reads expect g_mem to
    // hold the newest committed value.  Ordered before the state publish
    // so a successor that observes this entry COMMITTED in its Phase-1
    // wait also observes the folded-through values.
    for we in &tx.write_set {
        write_mem_val(we.addr, tv_to_u64(&we.value), byte_size_of_tv(&we.value));
    }

    e.state.store(LogState::Committed as u64, Ordering::Release);
    for we in &tx.write_set {
        index_insert(we.addr as u64, slot);
    }

    // Reclamation: fold the folded-out prefix into memory, then rotate.
    if slot - G_WM.load(Ordering::Relaxed) > KRECLAIM_THRESHOLD {
        for s in w..slot {
            let le = &G_LOG[s as usize & KLOG_MASK];
            if le.tag.load(Ordering::Acquire) != s {
                continue; // already folded by an earlier reclamation
            }
            if (le.state.load(Ordering::Acquire) as u32 & 0xFF) == LogState::Committed as u32 {
                let cnt = le.ws_count.load(Ordering::Relaxed).min(KMAX_INLINE_WS);
                for i in 0..cnt {
                    write_mem_val(
                        le.ws_addr[i].load(Ordering::Relaxed),
                        le.ws_val[i].load(Ordering::Relaxed),
                        8,
                    );
                }
            }
        }
        // Folded stores become visible to fast-path readers before the
        // filter is cleared.
        fence(Ordering::Release);
        G_WM.store(slot, Ordering::Release);
        bloom_clear();
    }

    // Mark this commit's writes dirty so later readers resolve via the log.
    for we in &tx.write_set {
        bloom_insert(we.addr as u64);
    }

    G_COMMIT_LOCK.store(0, Ordering::Release);

    tx.reset();
}

// ── Read / write ────────────────────────────────────────
fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }
    let sz = core::mem::size_of::<T>() as u8;

    // Read-own-writes: scan from the most recent entry.
    let ws_val = with_tx(|tx| {
        for e in tx.write_set.iter().rev() {
            if e.addr == addr {
                let esz = byte_size_of_tv(&e.value);
                if esz == sz {
                    return Some(T::from_typed(&e.value));
                }
            }
        }
        None
    });
    if let Some(v) = ws_val {
        return v;
    }

    // FAST PATH: clean Bloom miss — memory holds the newest committed value.
    if !bloom_contains(addr as u64) {
        fence(Ordering::Acquire);
        let val_u64 = read_mem_val(addr, sz);
        let val: T = match sz {
            1 => T::from_typed(&TypedValue::U8(val_u64 as u8)),
            2 => T::from_typed(&TypedValue::U16(val_u64 as u16)),
            4 => T::from_typed(&TypedValue::U32(val_u64 as u32)),
            8 => T::from_typed(&TypedValue::U64(val_u64)),
            _ => unreachable!(),
        };
        with_tx(|tx| {
            tx.read_set.push(ReadEntry {
                addr,
                sz,
                observed_val: val_u64,
            });
            #[cfg(feature = "stats")]
            TM_STATS.total_read_set_entries.fetch_add(1, Ordering::Relaxed);
        });
        return val;
    }

    // SLOW PATH: snoop the newest committed writer's log entry.
    let val_u64 = with_tx(|_tx| resolve(addr, sz));
    let val: T = match sz {
        1 => T::from_typed(&TypedValue::U8(val_u64 as u8)),
        2 => T::from_typed(&TypedValue::U16(val_u64 as u16)),
        4 => T::from_typed(&TypedValue::U32(val_u64 as u32)),
        8 => T::from_typed(&TypedValue::U64(val_u64)),
        _ => unreachable!(),
    };
    with_tx(|tx| {
        tx.read_set.push(ReadEntry {
            addr,
            sz,
            observed_val: val_u64,
        });
        #[cfg(feature = "stats")]
        TM_STATS.total_read_set_entries.fetch_add(1, Ordering::Relaxed);
    });
    val
}

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

        // Scan from end for existing entry at this address.
        for i in (0..tx.write_set.len()).rev() {
            if tx.write_set[i].addr == addr {
                let esz = byte_size_of_tv(&tx.write_set[i].value);
                if esz == sz {
                    tx.write_set[i].value = tv;
                    return;
                }
                if esz >= sz {
                    return;
                }
                break;
            }
        }

        tx.write_set.push(WriteEntry { addr, value: tv });
        #[cfg(feature = "stats")]
        TM_STATS.total_write_set_entries.fetch_add(1, Ordering::Relaxed);
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
        tx.write_set.retain(|e| e.addr != addr);
        tx.write_set.push(WriteEntry { addr, value: tv });
    });
}

// ── Public API ──────────────────────────────────────────
pub fn tm_init() {
    tm_install_tmx_hook();
    G_NEXT.store(1, Ordering::Release);
    G_WM.store(0, Ordering::Release);
    G_COMMIT_LOCK.store(0, Ordering::Release);
    TM_ABORT_COUNT.store(0, Ordering::Relaxed);
    bloom_clear();
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
    #[cfg(not(feature = "simulation"))]
    TX.with(|tx| {
        let mut b = tx.borrow_mut();
        if b.is_none() {
            *b = Some(Box::new(TxState::new()));
        }
        begin_impl(b.as_mut().unwrap());
    });
    #[cfg(feature = "simulation")]
    {
        let tid = runtime_core::current_sim_thread_id();
        let store = sim_tx_store();
        let mut map = store.lock().unwrap_or_else(|e| e.into_inner());
        let slot = map.get_mut(&tid).expect("no sim state for thread");
        if slot.is_none() {
            *slot = Some(Box::new(TxState::new()));
        }
        begin_impl(slot.as_mut().unwrap());
    }
}

pub fn tm_commit() -> bool {
    let mut tx = match flush_tx() {
        Some(t) => t,
        None => return true,
    };
    fence(Ordering::SeqCst);
    commit_impl(&mut tx);
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

// ── Simulation-only API (used by the TM simulator) ──────
#[cfg(feature = "simulation")]
pub mod sim {
    use super::*;

    /// Set the simulated thread ID for the current OS thread.
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
        s.validations
            .store(TM_STATS.validations.load(Ordering::Relaxed), Ordering::Relaxed);
        s.validation_failures.store(
            TM_STATS.validation_failures.load(Ordering::Relaxed),
            Ordering::Relaxed,
        );
        s.lock_contentions
            .store(TM_STATS.lock_contentions.load(Ordering::Relaxed), Ordering::Relaxed);
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
        eprintln!("  STATS (MVLog):");
        eprintln!(
            "    Commits={}  Aborts={}  Val={}  VFail={}  Locks={}  LAqFail={}  RS={}  WS={}",
            com, abt, val, vfail, lcon, laf, trs, tws
        );
    }
}
