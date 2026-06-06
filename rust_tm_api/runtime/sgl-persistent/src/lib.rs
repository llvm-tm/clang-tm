// ── SGL Persistent Rust backend ──────────────────────────
// Single Global Lock (atomic spinlock) with a file-backed TM
// region that survives process restarts.
//
// On tm_init(): the TM region is mmap'd from a persistent file.
// If the file already contains data, the TM region is restored
// (previous state is visible).  On tm_exit(): msync persists
// all changes to the file.
//
// Reads/writes are direct memory access — the lock serializes
// everything.  No write-set, no read-set, no aborts.

use core::sync::atomic::{AtomicBool, fence, Ordering};

pub use runtime_core::{Primitive, TypedValue};

const PERSIST_FILE: &str = "benchmark_results/tm_persist.bin";

// ── Global lock (spinlock) ──────────────────────────────
static GLOBAL_LOCK: AtomicBool = AtomicBool::new(false);

fn lock_global() {
    while GLOBAL_LOCK.swap(true, Ordering::Acquire) {
        while GLOBAL_LOCK.load(Ordering::Relaxed) {
            std::hint::spin_loop();
        }
    }
}

fn unlock_global() {
    GLOBAL_LOCK.store(false, Ordering::Release);
}

// ── Public API ──────────────────────────────────────────
pub fn tm_init() {
    addrspace::tm_region_init_file(PERSIST_FILE, false);
}

pub fn tm_exit() {
    let base = addrspace::tm_region_start();
    let size = addrspace::tm_region_size();
    if base != 0 && size != 0 {
        unsafe {
            libc::msync(base as *mut libc::c_void, size, libc::MS_SYNC);
        }
    }
}

pub fn tm_init_thread() {}
pub fn tm_exit_thread() {}

pub fn tm_begin() {
    lock_global();
    fence(Ordering::SeqCst);
}

pub fn tm_commit() -> bool {
    fence(Ordering::SeqCst);
    unlock_global();
    true
}

pub fn tm_abort() {
    fence(Ordering::SeqCst);
    unlock_global();
}

pub fn tm_abort_count() -> u64 { 0 }

// ── Read/write — direct memory access (SGL provides isolation) ──

macro_rules! def_read {
    ($n:ident, $t:ty) => {
        #[inline]
        pub fn $n(addr: *mut $t) -> $t {
            unsafe { addr.read() }
        }
    };
}
macro_rules! def_write {
    ($n:ident, $t:ty) => {
        #[inline]
        pub fn $n(addr: *mut $t, val: $t) {
            unsafe { addr.write(val); }
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
pub fn tm_read_ptr<T>(addr: *mut *mut T) -> *mut T { unsafe { addr.read() } }

#[inline]
pub fn tm_write_ptr<T>(addr: *mut *mut T, val: *mut T) { unsafe { addr.write(val); } }

#[inline]
pub fn tm_read_raw(addr: *mut u8, dst: &mut [u8]) { unsafe { std::ptr::copy_nonoverlapping(addr, dst.as_mut_ptr(), dst.len()); } }

#[inline]
pub fn tm_write_raw(addr: *mut u8, src: &[u8]) { unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr, src.len()); } }
