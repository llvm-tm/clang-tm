use std::sync::atomic::{AtomicU64, Ordering, fence};
use std::sync::OnceLock;

// ── Constants ───────────────────────────────────────────
const LOCK_MASK: u64 = 0xFF;
const VERSION_SHIFT: u64 = 8;
pub const TABLE_BITS: u64 = 20;
const TABLE_SIZE: usize = 1 << TABLE_BITS;

// Hash function for address → lock index
pub fn lock_index(addr: usize) -> usize {
    let h = (addr as u64).wrapping_mul(0x9E3779B97F4A7C15);
    (h >> (64 - TABLE_BITS)) as usize
}

// ── Lock table (global, lazy-init) ──────────────────────
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
        if cur & LOCK_MASK != 0 {
            return false; // already locked by another thread
        }
        self.data
            .compare_exchange_weak(cur, cur | 1, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }

    fn unlock_exclusive(&self) {
        // Increment version, clear lock bits
        let cur = self.data.load(Ordering::Relaxed);
        let ver = (cur & !LOCK_MASK) >> VERSION_SHIFT;
        self.data.store((ver + 1) << VERSION_SHIFT, Ordering::Release);
    }

    fn version(&self) -> u64 {
        let v = self.data.load(Ordering::Acquire);
        (v & !LOCK_MASK) >> VERSION_SHIFT
    }
}

static LOCK_TABLE: OnceLock<Box<[Lock]>> = OnceLock::new();

fn locks() -> &'static [Lock] {
    LOCK_TABLE.get_or_init(|| {
        (0..TABLE_SIZE).map(|_| Lock::new()).collect::<Vec<_>>().into_boxed_slice()
    })
}

pub fn init() {
    locks();
    G_CLOCK.store(0, Ordering::Release);
}

// ── Global clock ────────────────────────────────────────
static G_CLOCK: AtomicU64 = AtomicU64::new(0);

pub fn gc_snapshot() -> u64 {
    G_CLOCK.load(Ordering::Acquire)
}

pub fn gc_acquire() {
    loop {
        let cur = G_CLOCK.load(Ordering::Relaxed);
        if cur & 1 == 0
            && G_CLOCK
                .compare_exchange_weak(cur, cur | 1, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
        {
            return;
        }
        std::hint::spin_loop();
    }
}

pub fn gc_release_and_inc() {
    // fetch_add(1) clears bit 0 (lock) and increments version in one atomic step
    G_CLOCK.fetch_add(1, Ordering::Release);
}

// ── Lock helpers ────────────────────────────────────────
pub fn is_locked(addr: usize) -> bool {
    locks()[lock_index(addr)].is_locked()
}

pub fn read_version(addr: usize) -> u64 {
    locks()[lock_index(addr)].version()
}

fn version_at_index(idx: usize) -> u64 {
    locks()[idx].version()
}

fn unlock_at_index(idx: usize) {
    locks()[idx].unlock_exclusive();
}

fn try_lock_at_index(idx: usize) -> bool {
    locks()[idx].try_lock_exclusive()
}

// ── Memory operations (the only unavoidable unsafe) ─────
#[inline]
pub unsafe fn write_mem(addr: usize, val: u64, nbytes: u8) {
    assert!(runtime_core::is_tm_address(addr as *const u8), "Address not in TM address space");
    let ptr = addr as *mut u64;
    match nbytes {
        1 => (ptr as *mut u8).write(val as u8),
        2 => (ptr as *mut u16).write(val as u16),
        4 => (ptr as *mut u32).write(val as u32),
        8 => ptr.write(val),
        _ => panic!("write_mem: unsupported size {nbytes}"),
    }
}

// ── Commit protocol (word-level, works on flat arrays) ──
// Returns true if commit succeeded, false if conflict → retry.
pub fn commit(
    read_set: &[(usize, u64)],      // (addr, version_at_read_time)
    write_set: &[(usize, u64, u8)], // (addr, value, byte_size)
) -> bool {
    fence(Ordering::SeqCst);

    if write_set.is_empty() {
        return true; // read-only, no commit work needed
    }

    // 1. Acquire global clock (sets bit 0, serializing concurrent commits)
    gc_acquire();
    fence(Ordering::SeqCst);

    // 2. Lock all write-set addresses (sorted by lock index to avoid deadlock)
    //    Build a deduplicated list of lock indices
    let mut lock_idxs: Vec<usize> = write_set
        .iter()
        .map(|(addr, _, _)| lock_index(*addr))
        .collect();
    lock_idxs.sort_unstable();
    lock_idxs.dedup();

    for &idx in &lock_idxs {
        while !try_lock_at_index(idx) {
            std::hint::spin_loop();
        }
    }
    fence(Ordering::SeqCst);

    // 3. Validate read-set — every address must have the same version as when read
    let valid = read_set
        .iter()
        .all(|(addr, version)| version_at_index(lock_index(*addr)) == *version);

    if !valid {
        // Release locks, release clock, abort
        for &idx in &lock_idxs {
            unlock_at_index(idx);
        }
        gc_release_and_inc();
        return false;
    }

    // 4. Write-back buffered values
    unsafe {
        for &(addr, val, nbytes) in write_set {
            write_mem(addr, val, nbytes);
        }
    }
    fence(Ordering::SeqCst);

    // 5. Release write-locks
    for &idx in &lock_idxs {
        unlock_at_index(idx);
    }
    fence(Ordering::SeqCst);

    // 6. Release global clock (fetch_add clears lock bit + increments version)
    gc_release_and_inc();

    true
}
