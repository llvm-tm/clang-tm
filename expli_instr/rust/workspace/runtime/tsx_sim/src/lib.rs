// ── TSX Simulation Backend ──────────────────────────────────
// Models Intel TSX (RTM) for simulation/debugging without
// requiring RTM-capable hardware.
//
// Key design points:
// 1. Write-set: cache-line granularity HashMap.
// 2. Read-set: Bloom filter (simulating Intel's undocumented
//    approximate tracking, which also uses a bloom-like scheme
//    in the L1 cache — exact mechanism is microarchitecture-
//    specific and unpublished).
// 3. Capacity limits (Skylake defaults): 512 cache lines for
//    read-set (~32 KB L1), 128 lines for write-set (~8 KB L1).
//    Configurable via TSX_SIM_MAX_READ_LINES / MAX_WRITE_LINES
//    env vars.
// 4. Conflict detection: on commit, check all other threads'
//    bloom filters for our write-set lines. If a line was read
//    by another thread (bloom hit), that thread is aborted.
// 5. Timing estimation: virtual cycle counter accumulated per
//    operation using Skylake RTM cycle costs.
// 6. SGL fallback: when TSX capacity is exceeded or conflicts
//    cause too many aborts (configurable retry limit), falls
//    back to a global mutex.
//
// The backend exposes the same public API as all other Rust
// backends (tm_init, tm_begin, tm_read, etc.) and can be used
// by the simulator via the Backend enum.

use core::sync::atomic::{compiler_fence, AtomicU64, Ordering};
use std::collections::{HashMap, HashSet};
use std::sync::Mutex;

// ── Constants ───────────────────────────────────────────────

/// Cache line size (bytes).
const CACHE_LINE: u64 = 64;

/// Default max unique cache lines for the read-set.
///
/// Intel TSX tracks read-set entries using an L1-data-cache-based bloom
/// filter (microarchitecture-specific, ~128–256 line practical limit
/// before false-positive conflict-detection rate degrades).  Skylake
/// shows capacity aborts above ~140 read lines in practice.
const DEFAULT_MAX_READ_LINES: usize = 128;

/// Default max unique cache lines for the write-set.
///
/// Write-set tracking uses the L1 cache's store-monitoring mechanism.
/// 8-way associativity × 64 sets gives 512 theoretical max, but
/// practical useful limit is ~32–48 before associativity conflicts cause
/// capacity aborts.  Benchmarks with small data sets (bank, fuzz_counter)
/// typically use 1–4 write lines.
const DEFAULT_MAX_WRITE_LINES: usize = 32;

/// Default max RTM retries before SGL fallback (matches TSXSGL: 5).
const DEFAULT_MAX_RETRIES: u64 = 5;
/// Bloom filter size in bits (m).
const BLOOM_BITS: usize = 4096;

/// Number of hash functions (k) — currently 2, hardcoded in the insert/check logic.
#[allow(dead_code)]
const BLOOM_HASHES: usize = 2;

// ── Cycle costs (calibrated Broadwell-EP v4 measurements) ───

/// _xbegin() entry cost (cycles).
const COST_XBEGIN: u64 = 60;
/// _xend() commit cost (cycles) — store buffer flush.
const COST_XEND: u64 = 178;
/// _xabort() cost (cycles) — architectural state restore.
const COST_XABORT: u64 = 1500;
/// L1 cache read hit (cycles).
const COST_READ_L1: u64 = 5;
/// L1 cache write hit (cycles) — store buffer.
const COST_WRITE_L1: u64 = 6;
/// Mutex lock acquisition (cycles) — fast path.
const COST_MUTEX_LOCK: u64 = 75;
/// Mutex unlock (cycles).
const COST_MUTEX_UNLOCK: u64 = 75;
/// Cache line invalidation from another core (cycles) —
/// approximate cost of the MESI invalidation + TSX abort.
const COST_CONFLICT_ABORT: u64 = 2000;
/// Bloom filter check per address (cycles).
const COST_BLOOM_CHECK: u64 = 2;

/// SGL owner spin-wait cycle cost — _mm_pause() busy-loop while
/// another thread holds the SGL.  Measured from real TSXSGL profiling:
/// fuzz_counter 2t shows ~161k spin cycles across 2091 spins = ~77/ea.
const COST_SGL_SPIN_WAIT: u64 = 80;

// ── Helper: cache-line address ──────────────────────────────

#[inline]
fn cache_line_addr(addr: u64) -> u64 {
    addr & !(CACHE_LINE - 1)
}

// ── Bloom filter ────────────────────────────────────────────
// Simple double-hashing bloom filter matching Intel's
// approximate read-set tracking.

#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone, Debug)]
pub(crate) struct BloomFilter {
    bits: Vec<u64>,  // bit array stored as u64 words
    word_count: usize,
}

impl BloomFilter {
    fn new() -> Self {
        let word_count = (BLOOM_BITS + 63) / 64;
        BloomFilter {
            bits: vec![0u64; word_count],
            word_count,
        }
    }

    fn clear(&mut self) {
        for w in &mut self.bits {
            *w = 0;
        }
    }

    fn hash1(addr: u64) -> usize {
        (addr.wrapping_mul(0x9E3779B97F4A7C15) as usize) % BLOOM_BITS
    }

    fn hash2(addr: u64) -> usize {
        (addr.wrapping_mul(0xBF58476D1CE4E5B9).wrapping_add(0x9E3779B9) as usize) % BLOOM_BITS
    }

    fn insert(&mut self, addr: u64) {
        let h1 = Self::hash1(addr);
        let h2 = Self::hash2(addr);
        self.bits[h1 / 64] |= 1u64 << (h1 % 64);
        self.bits[h2 / 64] |= 1u64 << (h2 % 64);
    }

    fn might_contain(&self, addr: u64) -> bool {
        let h1 = Self::hash1(addr);
        let h2 = Self::hash2(addr);
        (self.bits[h1 / 64] & (1u64 << (h1 % 64))) != 0
            && (self.bits[h2 / 64] & (1u64 << (h2 % 64))) != 0
    }
}

// ── Per-thread TSX simulation state ─────────────────────────

#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone, Debug)]
pub struct CacheLineWrite {
    pub addr: u64,      // full address (for offset calculation)
    pub offset: u8,     // byte offset within cache line
    pub width: u8,      // 1, 2, 4, or 8 bytes
    pub value: [u8; 8], // up to 8 bytes, little-endian
}

#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[derive(Clone, Debug)]
pub struct TsxThreadState {
    /// Whether this thread is inside a transaction.
    active: bool,
    /// Set to true only when tm_commit() actually committed (not
    /// externally aborted).  Used by the nop-return path when another
    /// thread's commit already set active=false.
    committed_successfully: bool,
    /// True when the transaction is running in simulated TSX mode.
    /// False = SGL fallback (mutex-based).
    in_tsx: bool,
    /// Max _xbegin() retries before falling to SGL (default 5).
    max_retries: u64,
    /// Consecutive transaction aborts tracked across TxBegin/TxEnd
    /// cycles.  When >= max_retries, the next tm_begin() falls to SGL
    /// regardless of SGL_OWNER.  Reset on any successful commit.
    ///
    /// Artificial: real TSXSGL does NOT accumulate across transactions;
    /// each tm_begin() retries up to 5 _xbegin() calls independently.
    /// This cross-transaction counter exists because the SimEngine
    /// processes events sequentially — there is no "meanwhile" for two
    /// threads' retry loops to interleave naturally.  The counter
    /// emulates the effect of a thread experiencing max_retries
    /// consecutive _xbegin() failures (LOCK_BUSY or conflict) and
    /// falling to SGL, which would happen naturally in real hardware.
    persistent_retries: u64,
    read_bloom: BloomFilter,
    read_lines: HashSet<u64>,
    write_lines: HashSet<u64>,
    writes: Vec<CacheLineWrite>,
    cycles: u64,
    abort_count: u64,
    commit_count: u64,
    capacity_aborts: u64,
    conflict_aborts: u64,
    self_aborts: u64,
    explicit_aborts: u64,
    other_aborts: u64,
    fallback_count: u64,
    peak_read_set: usize,
    peak_write_set: usize,
}

impl TsxThreadState {
    fn new() -> Self {
        TsxThreadState {
            active: false,
            committed_successfully: false,
            in_tsx: false,
            max_retries: Self::max_retries(),
            persistent_retries: 0,
            read_bloom: BloomFilter::new(),
            read_lines: HashSet::new(),
            write_lines: HashSet::new(),
            writes: Vec::new(),
            cycles: 0,
            abort_count: 0,
            commit_count: 0,
            capacity_aborts: 0,
            conflict_aborts: 0,
            self_aborts: 0,
            explicit_aborts: 0,
            other_aborts: 0,
            fallback_count: 0,
            peak_read_set: 0,
            peak_write_set: 0,
        }
    }

    fn reset_tx(&mut self) {
        self.read_bloom.clear();
        self.read_lines.clear();
        self.write_lines.clear();
        self.writes.clear();
        self.committed_successfully = false;
    }

    fn max_read_lines() -> usize {
        std::env::var("TSX_SIM_MAX_READ_LINES")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(DEFAULT_MAX_READ_LINES)
    }

    fn max_write_lines() -> usize {
        std::env::var("TSX_SIM_MAX_WRITE_LINES")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(DEFAULT_MAX_WRITE_LINES)
    }

    fn max_retries() -> u64 {
        std::env::var("TSX_SIM_MAX_RETRIES")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(DEFAULT_MAX_RETRIES)
    }
}

// ── Global shared state (simulated "conflict bus") ──────────
// All simulated threads share this via Mutex.

static GLOBAL_STATE: std::sync::LazyLock<Mutex<HashMap<u64, TsxThreadState>>> =
    std::sync::LazyLock::new(|| Mutex::new(HashMap::new()));

/// Read config env vars once at init.
static CONFIG_MAX_READ: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
static CONFIG_MAX_WRITE: std::sync::OnceLock<usize> = std::sync::OnceLock::new();

fn max_read() -> usize {
    *CONFIG_MAX_READ.get_or_init(TsxThreadState::max_read_lines)
}

fn max_write() -> usize {
    *CONFIG_MAX_WRITE.get_or_init(TsxThreadState::max_write_lines)
}

/// Simulated SGL owner: 0 = free, non-zero = thread ID holds the lock.
/// Written by the thread that acquires the SGL fallback, read by every
/// `tm_begin()` retry loop iteration to simulate the LOCK_BUSY check.
static SGL_OWNER: AtomicU64 = AtomicU64::new(0);

// ── Thread management ─────────────────────────────────────

thread_local! {
    static THREAD_TID: std::cell::Cell<Option<u64>> = const { std::cell::Cell::new(None) };
}

fn my_tid() -> u64 {
    THREAD_TID.with(|c| c.get().expect("TSX Sim: thread_id not set"))
}

fn with_state<F, R>(tid: u64, f: F) -> R
where
    F: FnOnce(&mut TsxThreadState) -> R,
{
    let mut guard = GLOBAL_STATE.lock().unwrap();
    let state = guard.entry(tid).or_insert_with(TsxThreadState::new);
    f(state)
}

// ── Public API ──────────────────────────────────────────────

pub fn tm_init() {
    CONFIG_MAX_READ.get_or_init(TsxThreadState::max_read_lines);
    CONFIG_MAX_WRITE.get_or_init(TsxThreadState::max_write_lines);
}

pub fn tm_exit() {
    // Print aggregated stats
    let guard = GLOBAL_STATE.lock().unwrap();
    let mut total_commits = 0u64;
    let mut total_aborts = 0u64;
    let mut total_capacity = 0u64;
    let mut total_conflict = 0u64;
    let mut total_fallback = 0u64;
    for (_, s) in guard.iter() {
        total_commits += s.commit_count;
        total_aborts += s.abort_count;
        total_capacity += s.capacity_aborts;
        total_conflict += s.conflict_aborts;
        total_fallback += s.fallback_count;
    }
    eprintln!(
        "[TSX_SIM] commits={} aborts={} (capacity={}, conflict={}, self={}) fallback={}",
        total_commits, total_aborts, total_capacity, total_conflict,
        total_aborts - total_capacity - total_conflict,
        total_fallback
    );
}

pub fn tm_init_thread() {
    let tid = my_tid();
    with_state(tid, |s| *s = TsxThreadState::new());
}

pub fn tm_exit_thread() {
    let tid = my_tid();
    with_state(tid, |s| {
        s.active = false;
    });
}

// ── Begin ───────────────────────────────────────────────────
//
// Matches real TSXSGL semantics:
//   for attempt in 0..max_retries:
//     status = _xbegin()
//     if status == _XBEGIN_STARTED:
//       if sgl_owner != 0:
//         _xabort(LOCK_BUSY)  // explicit abort → retry
//       // TSX running
//       return
//     // _xbegin() failed — handle abort
//     if explicit && LOCK_BUSY: spin-wait
//     if !retryable: break
//   // Retries exhausted — SGL fallback
//   global_tx_lock.lock()
//   sgl_owner = our_tid

/// Single-attempt TSX begin: returns true if TSX entered successfully,
/// false if SGL_OWNER was held (LOCK_BUSY).  Does NOT retry or enter
/// SGL fallback.  The caller (SimEngine or standalone tm_begin()) is
/// responsible for retry and SGL fallback logic.
fn try_begin_single(tid: u64) -> bool {
    with_state(tid, |s| {
        s.reset_tx();

        let owner = SGL_OWNER.load(Ordering::Relaxed);
        if owner != 0 && owner != tid {
            s.cycles += COST_XBEGIN + COST_XABORT;
            s.abort_count += 1;
            s.explicit_aborts += 1;
            s.persistent_retries += 1;
            return false;
        }

        s.cycles += COST_XBEGIN;
        s.active = true;
        s.in_tsx = true;
        true
    })
}

/// Standalone tm_begin() with retry loop (for non-SimEngine usage).
/// Calls try_begin_single() in a loop.  After max_retries consecutive
/// failures, falls to SGL fallback by acquiring SGL_OWNER.
pub fn tm_begin() {
    let tid = my_tid();
    let max_r = TsxThreadState::max_retries();

    // Persistent-retry exhaustion → SGL fallback
    //
    // Artificial: real TSXSGL falls to SGL within one tm_begin()
    // after 5 xbegin failures.  In the SimEngine's sequential model,
    // events from other threads can't interleave between retries
    // within the same tm_begin() call.  This cross-transaction counter
    // emulates the effect: after max_retries consecutive committed
    // aborts, the next tm_begin() skips the retry loop and goes
    // straight to SGL.
    let should_sgl = with_state(tid, |s| s.persistent_retries >= max_r);

    if should_sgl {
        with_state(tid, |s| {
            s.active = true;
            s.in_tsx = false;
            s.fallback_count += 1;
            s.cycles += COST_MUTEX_LOCK;
        });
        SGL_OWNER.store(tid, Ordering::Release);
        return;
    }

    // Retry loop
    for _attempt in 0..max_r {
        if try_begin_single(tid) {
            return;
        }
    }

    // All local retries exhausted by LOCK_BUSY → enter TSX anyway.
    // OWNER_CHANGED at commit will catch the conflict.
    with_state(tid, |s| {
        s.active = true;
        s.in_tsx = true;
    });
}

// ── Commit ──────────────────────────────────────────────────

pub fn tm_commit() -> bool {
    let tid = my_tid();
    compiler_fence(Ordering::SeqCst);

    // Lock global state once for the entire commit operation
    let mut guard = GLOBAL_STATE.lock().unwrap();

    let state = guard.get_mut(&tid);
    let s = match state {
        Some(s) => s,
        None => return true,
    };

    if !s.active {
        // Transaction already ended — return whether it committed or not
        return s.committed_successfully;
    }
    s.committed_successfully = false; // will be set to true on actual commit below

    // SGL fallback: commit via mutex (no conflict detection), release SGL
    if !s.in_tsx {
        SGL_OWNER.store(0, Ordering::Release);
        s.cycles += COST_MUTEX_UNLOCK;
        s.active = false;
        s.committed_successfully = true;
        s.persistent_retries = 0;
        s.commit_count += 1;
        s.reset_tx();
        return true;
    }

    // ── SGL_OWNER check (OWNER_CHANGED) ────────────────────
    //
    // Matches real TSXSGL's guard at _xend():
    //   if (sgl_owner.load() != tsx_start_owner) _xabort(OWNER_CHANGED)
    //
    // If some other thread holds the SGL when we try to commit, their
    // mutex-protected writes may have invalidated our read-set.  We
    // must abort.  This is the mechanism that prevents TSX and SGL
    // mode from coexisting: once one thread falls to SGL, all TSX
    // transactions that overlap with its SGL window abort.
    //
    // Artificial: real TSXSGL stores tsx_start_owner at begin() time
    // and checks equality with the current sgl_owner.  We simplify
    // to checking whether ANY thread holds SGL.  The difference:
    // if the SGL holder is the SAME thread that's now committing TSX
    // (impossible in our model — one thread can't be in both modes),
    // or if the SGL holder released before we reached commit (in
    // which case owner=0 and we pass).  Both are correct simplifications
    // because the SimEngine's sequential event ordering means an SGL
    // thread will always release SGL_OWNER before the next TSX commit.
    if SGL_OWNER.load(Ordering::Relaxed) != 0 {
        s.active = false;
        s.committed_successfully = false;
        s.conflict_aborts += 1;
        s.abort_count += 1;
        s.persistent_retries += 1;
        s.cycles += COST_XABORT + COST_CONFLICT_ABORT;
        s.reset_tx();
        return false;
    }

    // ── Bloom / write-set conflict detection ──
    // Two types of conflict at commit:
    //
    //   WR (write-read)   — our write-set line is in another thread's
    //                       read-set (bloom filter).  That thread read
    //                       stale data → we abort THEM, our commit wins.
    //
    //   WW (write-write)  — our write-set overlaps another thread's
    //                       write-set on the same cache line.  Lost update →
    //                       both must abort.
    //
    // In real TSX the cache-coherence protocol causes the second _xend()
    // to abort (first writer's cache invalidation aborts the second).
    // Here we model the same semantics: the first committer's WW check
    // finds the overlap and aborts both.  The SimEngine re-issues TxBegin.
    let write_lines: Vec<u64> = s.write_lines.iter().copied().collect();
    let our_tid = tid;

    let mut ww_conflict = false;  // write-write → abort BOTH

    for (other_tid, other_state) in guard.iter_mut() {
        if *other_tid == our_tid { continue; }
        if !other_state.active { continue; }
        if !other_state.in_tsx { continue; }

        // WR check: our write vs their read → abort them only
        for wl in &write_lines {
            if other_state.read_bloom.might_contain(*wl) {
                other_state.active = false;
                other_state.committed_successfully = false;
                other_state.conflict_aborts += 1;
                other_state.abort_count += 1;
                other_state.persistent_retries += 1;
                other_state.cycles += COST_CONFLICT_ABORT;
                other_state.reset_tx();
                break;
            }
        }

        // WW check: write-set overlap → abort both
        // (if other_state was already aborted by WR above, reset_tx()
        //  cleared its write_lines → no match here)
        for other_wl in &other_state.write_lines {
            if write_lines.contains(other_wl) {
                ww_conflict = true;
                if other_state.active {
                    other_state.active = false;
                    other_state.committed_successfully = false;
                    other_state.conflict_aborts += 1;
                    other_state.abort_count += 1;
                    other_state.persistent_retries += 1;
                    other_state.cycles += COST_CONFLICT_ABORT;
                    other_state.reset_tx();
                }
                break;
            }
        }
    }

    if ww_conflict {
        // Write-write: our transaction must also abort
        let our_state = guard.get_mut(&our_tid).unwrap();
        our_state.active = false;
        our_state.committed_successfully = false;
        our_state.conflict_aborts += 1;
        our_state.abort_count += 1;
        our_state.cycles += COST_XABORT + COST_CONFLICT_ABORT;
        our_state.persistent_retries += 1;
        our_state.reset_tx();
        return false;
    }

    // No WW conflict — commit successfully.
    // (WR conflicts already aborted the other threads; our writes win.)
    let our_state = guard.get_mut(&our_tid).unwrap();
    our_state.persistent_retries = 0;
    our_state.cycles += COST_XEND;
    our_state.active = false;
    our_state.committed_successfully = true;
    our_state.commit_count += 1;
    our_state.peak_read_set = our_state.peak_read_set.max(our_state.read_lines.len());
    our_state.peak_write_set = our_state.peak_write_set.max(our_state.write_lines.len());
    our_state.reset_tx();
    true
}

// ── Abort ───────────────────────────────────────────────────

pub fn tm_abort() {
    let tid = my_tid();
    with_state(tid, |s| {
        if !s.active { return; }
        // Release SGL owner if we held it (SGL fallback mode)
        if !s.in_tsx {
            SGL_OWNER.store(0, Ordering::Release);
        }
        s.self_aborts += 1;
        s.abort_count += 1;
        s.persistent_retries += 1;
        s.cycles += COST_XABORT;
        s.active = false;
        s.reset_tx();
    });
}

pub fn tm_abort_count() -> u64 {
    let tid = my_tid();
    with_state(tid, |s| s.abort_count)
}

// ── Read hooks ──────────────────────────────────────────────

macro_rules! def_read {
    ($n:ident, $t:ty) => {
        #[inline]
        pub fn $n(addr: *mut $t) -> $t {
            let tid = my_tid();
            let cl = cache_line_addr(addr as u64);
            with_state(tid, |s| {
                if s.active && s.in_tsx {
                    // Check write-set first (read-after-write)
                    for w in &s.writes {
                        if w.addr == addr as u64 {
                            s.cycles += COST_READ_L1 + COST_BLOOM_CHECK;
                            let bytes = w.value;
                            return unsafe { std::ptr::read_unaligned(bytes.as_ptr() as *const $t) };
                        }
                    }

                    // Track in read-set
                    s.read_bloom.insert(cl);
                    s.read_lines.insert(cl);

                    // Check capacity
                    if s.read_lines.len() > max_read() {
                        // Capacity abort
                        s.active = false;
                        s.capacity_aborts += 1;
                        s.abort_count += 1;
                        s.cycles += COST_XABORT;
                        s.reset_tx();
                        // Still return the actual memory value
                        return unsafe { addr.read() };
                    }

                    s.cycles += COST_READ_L1 + COST_BLOOM_CHECK;
                }
                unsafe { addr.read() }
            })
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

#[inline]
pub fn tm_read_ptr<T>(addr: *mut *mut T) -> *mut T {
    let tid = my_tid();
    let cl = cache_line_addr(addr as u64);
    with_state(tid, |s| {
        if s.active && s.in_tsx {
            for w in &s.writes {
                if w.addr == addr as u64 {
                    s.cycles += COST_READ_L1 + COST_BLOOM_CHECK;
                    let bytes = w.value;
                    return unsafe { std::ptr::read_unaligned(bytes.as_ptr() as *const *mut T) };
                }
            }
            s.read_bloom.insert(cl);
            s.read_lines.insert(cl);
            if s.read_lines.len() > max_read() {
                        s.active = false;
                        s.capacity_aborts += 1;
                        s.abort_count += 1;
                        s.cycles += COST_XABORT;
                        s.reset_tx();
                        return unsafe { addr.read() };
                    }

                    s.cycles += COST_READ_L1 + COST_BLOOM_CHECK;
                }
                unsafe { addr.read() }
    })
}

// ── Write hooks ─────────────────────────────────────────────

macro_rules! def_write {
    ($n:ident, $t:ty) => {
        #[inline]
        pub fn $n(addr: *mut $t, val: $t) {
            let tid = my_tid();
            let cl = cache_line_addr(addr as u64);
            with_state(tid, |s| {
                if s.active && s.in_tsx {
                    let offset = (addr as u64) & (CACHE_LINE - 1);
                    let width = std::mem::size_of::<$t>() as u8;

                    // Pack value into bytes (little-endian)
                    let mut bytes = [0u8; 8];
                    let val_bytes = unsafe {
                        std::slice::from_raw_parts(&val as *const $t as *const u8, width as usize)
                    };
                    bytes[..width as usize].copy_from_slice(val_bytes);

                    // Update or add write-set entry
                    let mut found = false;
                    for w in &mut s.writes {
                        if w.addr == addr as u64 {
                            w.value = bytes;
                            w.width = width;
                            found = true;
                            break;
                        }
                    }
                    if !found {
                        s.writes.push(CacheLineWrite {
                            addr: addr as u64,
                            offset: offset as u8,
                            width,
                            value: bytes,
                        });
                        s.write_lines.insert(cl);
                        // Simulate RMW: also adds to read-set
                        s.read_bloom.insert(cl);
                        s.read_lines.insert(cl);
                    }

                    s.peak_write_set = s.peak_write_set.max(s.write_lines.len());
                    s.peak_read_set = s.peak_read_set.max(s.read_lines.len());

                    // Check capacity
                    if s.write_lines.len() > max_write()
                        || s.read_lines.len() > max_read()
                    {
                        s.active = false;
                        s.capacity_aborts += 1;
                        s.abort_count += 1;
                        s.cycles += COST_XABORT;
                        s.reset_tx();
                        return;
                    }

                    s.cycles += COST_WRITE_L1 + COST_BLOOM_CHECK;
                }
                unsafe { addr.write(val); }
            });
        }
    };
}

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
pub fn tm_write_ptr<T>(addr: *mut *mut T, val: *mut T) {
    let tid = my_tid();
    let cl = cache_line_addr(addr as u64);
    with_state(tid, |s| {
        if s.active && s.in_tsx {
            let offset = (addr as u64) & (CACHE_LINE - 1);
            let mut bytes = [0u8; 8];
            let ptr_bytes = unsafe {
                std::slice::from_raw_parts(&val as *const *mut T as *const u8, 8)
            };
            bytes.copy_from_slice(ptr_bytes);
            let mut found = false;
            for w in &mut s.writes {
                if w.addr == addr as u64 {
                    w.value = bytes;
                    found = true;
                    break;
                }
            }
            if !found {
                s.writes.push(CacheLineWrite {
                    addr: addr as u64,
                    offset: offset as u8,
                    width: 8,
                    value: bytes,
                });
                s.write_lines.insert(cl);
                s.read_bloom.insert(cl);
                s.read_lines.insert(cl);
            }
            if s.write_lines.len() > max_write() || s.read_lines.len() > max_read() {
                s.active = false;
                s.capacity_aborts += 1;
                s.abort_count += 1;
                s.cycles += COST_XABORT;
                s.reset_tx();
                return;
            }
            s.cycles += COST_WRITE_L1 + COST_BLOOM_CHECK;
        }
        unsafe { addr.write(val); }
    });
}

// ── Raw memory operations ───────────────────────────────────

#[inline]
pub fn tm_read_raw(addr: *mut u8, dst: &mut [u8]) {
    let tid = my_tid();
    let cl = cache_line_addr(addr as u64);
    with_state(tid, |s| {
        if s.active && s.in_tsx {
            s.read_bloom.insert(cl);
            s.read_lines.insert(cl);
            if s.read_lines.len() > max_read() {
                s.active = false;
                s.capacity_aborts += 1;
                s.abort_count += 1;
                s.cycles += COST_XABORT;
                s.reset_tx();
                unsafe { std::ptr::copy_nonoverlapping(addr, dst.as_mut_ptr(), dst.len()); }
                return;
            }
            s.cycles += COST_READ_L1 + COST_BLOOM_CHECK;
        }
        unsafe { std::ptr::copy_nonoverlapping(addr, dst.as_mut_ptr(), dst.len()); }
    });
}

#[inline]
pub fn tm_write_raw(addr: *mut u8, src: &[u8]) {
    let tid = my_tid();
    let cl = cache_line_addr(addr as u64);
    with_state(tid, |s| {
        if s.active && s.in_tsx {
            let offset = (addr as u64) & (CACHE_LINE - 1);
            let mut bytes = [0u8; 8];
            let copy_len = src.len().min(8);
            bytes[..copy_len].copy_from_slice(&src[..copy_len]);
            let mut found = false;
            for w in &mut s.writes {
                if w.addr == addr as u64 {
                    w.value = bytes;
                    w.width = copy_len as u8;
                    found = true;
                    break;
                }
            }
            if !found {
                s.writes.push(CacheLineWrite {
                    addr: addr as u64,
                    offset: offset as u8,
                    width: copy_len as u8,
                    value: bytes,
                });
                s.write_lines.insert(cl);
                s.read_bloom.insert(cl);
                s.read_lines.insert(cl);
            }
            if s.write_lines.len() > max_write() || s.read_lines.len() > max_read() {
                s.active = false;
                s.capacity_aborts += 1;
                s.abort_count += 1;
                s.cycles += COST_XABORT;
                s.reset_tx();
                unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr, src.len()); }
                return;
            }
            s.cycles += COST_WRITE_L1 + COST_BLOOM_CHECK;
        }
        unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr, src.len()); }
    });
}

// ── Simulation module (for simulator integration) ───────────

#[cfg(feature = "simulation")]
pub mod sim {
    use super::*;

    pub fn set_thread_id(id: u64) {
        runtime_core::set_sim_thread_id(id);
        THREAD_TID.with(|c| c.set(Some(id)));
    }

    pub fn clear_thread_id() {
        runtime_core::clear_sim_thread_id();
        THREAD_TID.with(|c| c.set(None));
    }

    /// Single-attempt TSX begin, for the SimEngine's retry loop.
    /// Returns true if TSX was entered, false if LOCK_BUSY.
    /// Does NOT charge persistent_retries (SimEngine manages that).
    pub fn try_begin() -> bool {
        let tid = my_tid();
        with_state(tid, |s| {
            s.reset_tx();
            let owner = SGL_OWNER.load(Ordering::Relaxed);
            if owner != 0 && owner != tid {
                s.cycles += COST_XBEGIN + COST_XABORT;
                s.explicit_aborts += 1;
                s.abort_count += 1;
                return false;
            }
            s.cycles += COST_XBEGIN;
            s.active = true;
            s.in_tsx = true;
            true
        })
    }

    /// Force SGL fallback: acquires SGL_OWNER and sets the thread
    /// to SGL mode.  Called by the SimEngine after max_retries
    /// consecutive try_begin() failures.
    pub fn force_sgl() {
        let tid = my_tid();
        SGL_OWNER.store(tid, Ordering::Release);
        with_state(tid, |s| {
            s.active = true;
            s.in_tsx = false;
            s.fallback_count += 1;
            s.cycles += COST_MUTEX_LOCK;
        });
    }

    pub fn snapshot_states() -> std::collections::HashMap<u64, Option<Box<TsxThreadState>>> {
        let guard = GLOBAL_STATE.lock().unwrap();
        guard.iter().map(|(&k, v)| {
            (k, Some(Box::new(v.clone())))
        }).collect()
    }

    pub fn restore_states(states: std::collections::HashMap<u64, Option<Box<TsxThreadState>>>) {
        let mut guard = GLOBAL_STATE.lock().unwrap();
        guard.clear();
        for (k, v) in states {
            if let Some(state) = v {
                guard.insert(k, *state);
            }
        }
    }

    pub fn reset() {
        let Some(tid) = runtime_core::try_current_sim_thread_id() else { return; };
        let mut guard = GLOBAL_STATE.lock().unwrap();
        guard.remove(&tid);
    }

    #[cfg(feature = "stats")]
    pub fn take_stats() -> runtime_core::SyncCounters {
        let s = runtime_core::SyncCounters::new();
        let guard = GLOBAL_STATE.lock().unwrap();
        let mut commits = 0u64;
        let mut aborts = 0u64;
        let mut conflict = 0u64;
        let mut capacity = 0u64;
        let mut self_ab = 0u64;
        let mut explicit = 0u64;
        let mut fallback = 0u64;
        for (_, ts) in guard.iter() {
            commits += ts.commit_count;
            aborts += ts.abort_count;
            conflict += ts.conflict_aborts;
            capacity += ts.capacity_aborts;
            self_ab += ts.self_aborts;
            explicit += ts.explicit_aborts;
            fallback += ts.fallback_count;
        }
        s.commits.store(commits, Ordering::Relaxed);
        s.aborts.store(aborts, Ordering::Relaxed);
        s.sim_conflict.store(conflict, Ordering::Relaxed);
        s.sim_capacity.store(capacity, Ordering::Relaxed);
        s.sim_self.store(self_ab, Ordering::Relaxed);
        s.sim_explicit.store(explicit, Ordering::Relaxed);
        s.sim_fallback.store(fallback, Ordering::Relaxed);
        s
    }

    #[cfg(feature = "stats")]
    pub fn print_stats(s: &runtime_core::SyncCounters) {
        let commits = s.commits.load(Ordering::Relaxed);
        let aborts = s.aborts.load(Ordering::Relaxed);
        let conflict = s.sim_conflict.load(Ordering::Relaxed);
        let capacity = s.sim_capacity.load(Ordering::Relaxed);
        let self_ab = s.sim_self.load(Ordering::Relaxed);
        let explicit = s.sim_explicit.load(Ordering::Relaxed);
        let fallback = s.sim_fallback.load(Ordering::Relaxed);
        let other = aborts.saturating_sub(conflict + capacity + self_ab + explicit);
        eprintln!("  STATS (TSX SIM):");
        eprintln!("    Commits={} Aborts={} (rate={:.1}%)",
                  commits, aborts,
                  if commits + aborts > 0 { 100.0 * aborts as f64 / (commits + aborts) as f64 } else { 0.0 });
        eprintln!("    Abort breakdown: conflict={} capacity={} explicit={} self={} other={} fallback={}",
                  conflict, capacity, explicit, self_ab, other, fallback);
    }
}
