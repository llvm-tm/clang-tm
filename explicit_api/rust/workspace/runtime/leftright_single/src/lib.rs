// ── LeftRight Phase-Lock TM Backend ──────────────────────────────

// Safety contract (review-05 R-09): the `tm_read_*`/`tm_write_*` entry
// points below dereference raw pointers inside otherwise-safe functions.
// This mirrors the C++ hook ABI (tm_read_i1/tm_write_i8/...): callers —
// the LLVM instrumentation pipeline or the explicit-API test drivers —
// guarantee every pointer is aligned, correctly sized for its access
// width, and live for the duration of the access. Passing arbitrary or
// dangling pointers through these functions is UB by contract.
#![allow(clippy::not_unsafe_ptr_arg_deref)]
//
// True Left-Right (Ramalhete/Correia 2015) for TM with phase-based writes:
//   - Phase 1 (TX body): writer holds a global lock, writes eagerly to the
//     INACTIVE copy.  Readers are wait-free and read from the ACTIVE copy.
//   - Phase 2 (commit): writer flips ACTIVE, waits for old readers to drain,
//     syncs the old copy, writes back to memory, releases the lock.
//
// Two reader counters (classic Left-Right) ensure the barrier terminates:
// the writer waits only for readers that saw the pre-flip ACTIVE value.
// New readers after the flip are tracked by the other counter and never
// delay the writer.

use std::cell::{RefCell, UnsafeCell};
use std::collections::HashMap;
use std::hint;
use std::ptr;
use std::sync::atomic::{fence, AtomicBool, AtomicU64, Ordering};
use std::sync::LazyLock;

// ── Write entry ─────────────────────────────────────────────────
#[derive(Clone, Copy)]
struct WriteEntry {
    addr: usize,
    val: u64,
    // None = address absent from the shadow map before this write; undo must
    // remove the key again, not insert 0 (which would shadow memory forever
    // — review-05 R-04).
    old_val: Option<u64>,
    bytes: u8,
}

// ── Unsafe HashMap wrapper ───────────────────────────────────────
struct SyncMap(UnsafeCell<HashMap<usize, u64>>);
unsafe impl Sync for SyncMap {}

impl SyncMap {
    fn new() -> Self {
        SyncMap(UnsafeCell::new(HashMap::new()))
    }
    fn read(&self, addr: usize) -> Option<u64> {
        unsafe { (*self.0.get()).get(&addr).copied() }
    }
    fn write(&self, addr: usize, val: u64) {
        unsafe {
            (*self.0.get()).insert(addr, val);
        }
    }
    fn remove(&self, addr: usize) {
        unsafe {
            (*self.0.get()).remove(&addr);
        }
    }
    fn as_ptr(&self) -> *mut HashMap<usize, u64> {
        self.0.get()
    }
}

// ── Globals ──────────────────────────────────────────────────────
static LEFT: LazyLock<SyncMap> = LazyLock::new(SyncMap::new);
static RIGHT: LazyLock<SyncMap> = LazyLock::new(SyncMap::new);
static ACTIVE: AtomicBool = AtomicBool::new(false); // false = LEFT
static LEFT_READERS: AtomicU64 = AtomicU64::new(0);
static RIGHT_READERS: AtomicU64 = AtomicU64::new(0);

// ── Thread-local state ───────────────────────────────────────────
thread_local! {
    static WRITE_SET: RefCell<Vec<WriteEntry>> = const { RefCell::new(Vec::new()) };
    static LOCK_HELD: RefCell<bool> = const { RefCell::new(false) };
}

unsafe fn write_memory(addr: usize, val: u64, bytes: u8) {
    let ptr = addr as *mut u8;
    match bytes {
        1 => ptr::write(ptr, val as u8),
        2 => ptr::write(ptr as *mut u16, val as u16),
        4 => ptr::write(ptr as *mut u32, val as u32),
        8 => ptr::write(ptr as *mut u64, val),
        _ => unreachable!(),
    }
}

// ── Init / Exit ──────────────────────────────────────────────────
pub fn tm_init() {
    unsafe { LEFT.as_ptr().as_mut() }.unwrap().clear();
    unsafe { RIGHT.as_ptr().as_mut() }.unwrap().clear();
    ACTIVE.store(false, Ordering::Release);
    LEFT_READERS.store(0, Ordering::Release);
    RIGHT_READERS.store(0, Ordering::Release);
}

pub fn tm_exit() {}

pub fn tm_init_thread() {}
pub fn tm_exit_thread() {}

pub fn tm_abort_count() -> u64 {
    0
}

// ── Write lock (spinlock) ───────────────────────────────────────
// The lock serializes write transactions.  Held from first tm_write
// through tm_commit/tm_abort.  Reentrant within the same TX.

static LOCK: AtomicU64 = AtomicU64::new(0);

fn write_lock_acquire() {
    if LOCK_HELD.with(|h| *h.borrow()) {
        return;
    }
    while LOCK
        .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        hint::spin_loop();
    }
    LOCK_HELD.with(|h| *h.borrow_mut() = true);
}

fn write_lock_release() {
    if !LOCK_HELD.with(|h| *h.borrow()) {
        return;
    }
    LOCK.store(0, Ordering::Release);
    LOCK_HELD.with(|h| *h.borrow_mut() = false);
}

// ── Begin / Abort / Commit ──────────────────────────────────────
pub fn tm_begin() {
    WRITE_SET.with(|ws| ws.borrow_mut().clear());
    LOCK_HELD.with(|h| *h.borrow_mut() = false);
}

pub fn tm_abort() {
    // Undo all writes to the inactive copy
    let ws = WRITE_SET.with(|ws| std::mem::take(&mut *ws.borrow_mut()));
    if ws.is_empty() {
        write_lock_release();
        return;
    }

    // Determine which copy is inactive (the one we wrote to)
    // Since we hold the write lock, no other writer has flipped ACTIVE.
    let inactive = if ACTIVE.load(Ordering::Relaxed) {
        &*LEFT
    } else {
        &*RIGHT
    };

    // Restore old values newest-first: repeated writes to one address form
    // a chain and forward order would leave an intermediate value (04/R-01
    // class); an absent key is restored by removing it again.
    for e in ws.iter().rev() {
        match e.old_val {
            Some(v) => inactive.write(e.addr, v),
            None => inactive.remove(e.addr),
        }
    }

    write_lock_release();
}

pub fn tm_commit() -> bool {
    let ws = WRITE_SET.with(|ws| std::mem::take(&mut *ws.borrow_mut()));
    if ws.is_empty() {
        write_lock_release();
        return true;
    }

    // Capture ACTIVE before any potential flip — the lock guarantees
    // no concurrent flip by another writer.
    let active_was_left = !ACTIVE.load(Ordering::Relaxed);

    // ── Step 1: Writes already applied to inactive copy during TX ──

    // ── Step 2: Flip ──
    ACTIVE.store(!active_was_left, Ordering::Release);
    fence(Ordering::SeqCst);

    // ── Step 3: Barrier — wait for old readers ──
    // Readers that started before the flip saw the OLD active value.
    // They're tracked by their respective counter.
    if active_was_left {
        // Old active was LEFT — wait for LEFT readers to drain
        while LEFT_READERS.load(Ordering::Acquire) > 0 {
            hint::spin_loop();
        }
    } else {
        // Old active was RIGHT — wait for RIGHT readers to drain
        while RIGHT_READERS.load(Ordering::Acquire) > 0 {
            hint::spin_loop();
        }
    }

    // ── Step 4: Sync old active (now inactive) from new active ──
    let dst = if active_was_left { &*LEFT } else { &*RIGHT }; // old active, now inactive
    let src = if active_was_left { &*RIGHT } else { &*LEFT }; // new active
    for &e in &ws {
        if let Some(v) = src.read(e.addr) {
            dst.write(e.addr, v);
        }
    }

    // ── Step 5: Write back to original memory ──
    // Safe: no old reader is in-flight (barrier passed).  New readers read
    // from the new active copy, not from memory.
    for &e in &ws {
        unsafe {
            write_memory(e.addr, e.val, e.bytes);
        }
    }

    // ── Step 6: Release write lock ──
    write_lock_release();

    true
}

// ── Read word ────────────────────────────────────────────────────
fn read_word_bytes(addr: usize, bytes: u8) -> u64 {
    // Read-your-writes: check thread-local write set first
    if let Some(v) = WRITE_SET.with(|ws| {
        ws.borrow()
            .iter()
            .rev()
            .find(|e| e.addr == addr)
            .map(|e| e.val)
    }) {
        return v;
    }

    // Reader-enter (review-05 R-04): register on the counter BEFORE
    // observing ACTIVE, then re-check ACTIVE; if it flipped while we were
    // registering, the writer may drain a counter that did not yet include
    // us, so retry.  Increment → fence → check is the classic LeftRight
    // handshake; loading ACTIVE first (as here used to) allows a writer to
    // mutate the map we are about to read.
    let (val, active);
    loop {
        let seen = ACTIVE.load(Ordering::Acquire);
        if seen {
            RIGHT_READERS.fetch_add(1, Ordering::Acquire);
        } else {
            LEFT_READERS.fetch_add(1, Ordering::Acquire);
        }
        fence(Ordering::SeqCst);
        if ACTIVE.load(Ordering::Acquire) == seen {
            active = seen;
            break;
        }
        // Flipped during registration: back off and retry.
        if seen {
            RIGHT_READERS.fetch_sub(1, Ordering::Release);
        } else {
            LEFT_READERS.fetch_sub(1, Ordering::Release);
        }
    }

    // Read from the active copy
    val = if active {
        RIGHT.read(addr)
    } else {
        LEFT.read(addr)
    };

    // Reader-exit
    if active {
        RIGHT_READERS.fetch_sub(1, Ordering::Release);
    } else {
        LEFT_READERS.fetch_sub(1, Ordering::Release);
    }

    // Fall back to memory if this address has never been written
    match val {
        Some(v) => v,
        None => unsafe {
            match bytes {
                1 => *(addr as *const u8) as u64,
                2 => *(addr as *const u16) as u64,
                4 => *(addr as *const u32) as u64,
                8 => *(addr as *const u64),
                _ => 0,
            }
        },
    }
}

// ── Write word (eager: applies to inactive copy immediately) ─────
fn write_word(addr: usize, val: u64, bytes: u8) {
    write_lock_acquire();

    // Determine inactive copy (the one we write to — no readers touch it)
    let inactive = if ACTIVE.load(Ordering::Relaxed) {
        &*LEFT
    } else {
        &*RIGHT
    };

    // Save old value for undo; None means "absent from the map" (memory
    // fallback), not 0.
    let old_val = inactive.read(addr);

    // Write to inactive copy
    inactive.write(addr, val);

    // Record in write set for commit/abort
    WRITE_SET.with(|ws| {
        ws.borrow_mut().push(WriteEntry {
            addr,
            val,
            old_val,
            bytes,
        });
    });
}

// ── Typed read/write ────────────────────────────────────────────
macro_rules! def_read {
    ($n:ident, $t:ty, $bytes:expr) => {
        #[inline]
        pub fn $n(addr: *mut $t) -> $t {
            read_word_bytes(addr as usize, $bytes) as $t
        }
    };
}

def_read!(tm_read_u8, u8, 1);
def_read!(tm_read_u16, u16, 2);
def_read!(tm_read_u32, u32, 4);
def_read!(tm_read_u64, u64, 8);
def_read!(tm_read_i8, i8, 1);
def_read!(tm_read_i16, i16, 2);
def_read!(tm_read_i32, i32, 4);
def_read!(tm_read_i64, i64, 8);

pub fn tm_read_f32(addr: *mut f32) -> f32 {
    f32::from_bits(read_word_bytes(addr as usize, 4) as u32)
}
pub fn tm_read_f64(addr: *mut f64) -> f64 {
    f64::from_bits(read_word_bytes(addr as usize, 8))
}

pub fn tm_write_u8(addr: *mut u8, val: u8) {
    write_word(addr as usize, val as u64, 1);
}
pub fn tm_write_u16(addr: *mut u16, val: u16) {
    write_word(addr as usize, val as u64, 2);
}
pub fn tm_write_u32(addr: *mut u32, val: u32) {
    write_word(addr as usize, val as u64, 4);
}
pub fn tm_write_u64(addr: *mut u64, val: u64) {
    write_word(addr as usize, val, 8);
}
pub fn tm_write_i8(addr: *mut i8, val: i8) {
    write_word(addr as usize, val as u64, 1);
}
pub fn tm_write_i16(addr: *mut i16, val: i16) {
    write_word(addr as usize, val as u64, 2);
}
pub fn tm_write_i32(addr: *mut i32, val: i32) {
    write_word(addr as usize, val as u64, 4);
}
pub fn tm_write_i64(addr: *mut i64, val: i64) {
    write_word(addr as usize, val as u64, 8);
}
pub fn tm_write_f32(addr: *mut f32, val: f32) {
    write_word(addr as usize, val.to_bits() as u64, 4);
}
pub fn tm_write_f64(addr: *mut f64, val: f64) {
    write_word(addr as usize, val.to_bits(), 8);
}

pub fn tm_read_ptr<T>(addr: *mut *mut T) -> *mut T {
    read_word_bytes(addr as usize, 8) as *mut T
}
pub fn tm_write_ptr<T>(addr: *mut *mut T, val: *mut T) {
    write_word(addr as usize, val as u64, 8);
}

pub fn tm_read_raw(addr: *mut u8, dst: &mut [u8]) {
    for (i, d) in dst.iter_mut().enumerate() {
        *d = read_word_bytes(addr as usize + i, 1) as u8;
    }
}
pub fn tm_write_raw(addr: *mut u8, src: &[u8]) {
    for (i, &s) in src.iter().enumerate() {
        write_word(addr as usize + i, s as u64, 1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static LR_LOCK: Mutex<()> = Mutex::new(());

    // R-04: aborting a transaction that first-wrote an address must not
    // leave a shadowing entry (old implementation inserted 0 forever).
    #[test]
    fn abort_first_write_does_not_shadow_memory() {
        let _g = LR_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a = Box::into_raw(Box::new(0x77u64));
        tm_begin();
        tm_write_u64(a, 5);
        tm_abort();
        unsafe {
            assert_eq!(*a, 0x77, "memory must be untouched");
        }
        assert_eq!(
            tm_read_u64(a),
            0x77,
            "a never-written address must fall back to memory after abort, not read a shadowed 0"
        );
        unsafe { drop(Box::from_raw(a)) };
    }

    // R-01 class: repeated writes undo in reverse (chain back to pre-TX value).
    #[test]
    fn repeated_writes_abort_restores_pre_value() {
        let _g = LR_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a = Box::into_raw(Box::new(0x77u64));
        tm_begin();
        tm_write_u64(a, 1);
        tm_write_u64(a, 2);
        tm_write_u64(a, 3);
        tm_abort();
        unsafe {
            assert_eq!(*a, 0x77);
        }
        assert_eq!(tm_read_u64(a), 0x77);
        unsafe { drop(Box::from_raw(a)) };
    }

    #[test]
    fn commit_publishes_and_reads_agree() {
        let _g = LR_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a = Box::into_raw(Box::new(0x77u64));
        let b = Box::into_raw(Box::new(0x11u64));
        tm_begin();
        assert_eq!(tm_read_u64(a), 0x77);
        tm_write_u64(a, 42);
        assert_eq!(tm_read_u64(a), 42, "read-your-writes");
        assert!(tm_commit());
        assert_eq!(tm_read_u64(a), 42);
        assert_eq!(tm_read_u64(b), 0x11);
        unsafe {
            assert_eq!(*a, 42);
            drop(Box::from_raw(a));
            drop(Box::from_raw(b));
        }
    }

    #[test]
    fn abort_partial_then_commit_full() {
        let _g = LR_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        tm_init();
        let a = Box::into_raw(Box::new(10u64));
        tm_begin();
        tm_write_u64(a, 20);
        tm_abort();
        assert_eq!(tm_read_u64(a), 10);
        tm_begin();
        tm_write_u64(a, 30);
        assert!(tm_commit());
        assert_eq!(tm_read_u64(a), 30);
        unsafe {
            assert_eq!(*a, 30);
            drop(Box::from_raw(a));
        }
    }
}
