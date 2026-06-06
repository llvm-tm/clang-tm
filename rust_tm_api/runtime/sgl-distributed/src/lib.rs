// ── SGL Distributed Rust backend ────────────────────────
// Single Global Lock across multiple processes.
//
// Each process has its own private TM region (anonymous mmap).
// A shared file (MAP_SHARED) holds only the lock, barrier, and
// epoch — NOT the TM data itself.  Processes coordinate via
// the lock and barrier, but their TmCell allocated data is
// private.
//
// Environment:
//   TM_NPROCESSES - how many processes must reach the barrier
//                   before transactions can start (default 1).
//   TM_SHM_FILE   - path to the shared state file
//                   (default benchmark_results/tm_2pc_state.bin)

use core::sync::atomic::{fence, Ordering};

pub use runtime_core::{Primitive, TypedValue};

// ── Shared state layout (at offset 0 in the mmap file) ──
#[repr(C, align(64))]
struct SharedState {
    lock_flag: std::sync::atomic::AtomicU8,
    epoch: std::sync::atomic::AtomicI64,
    barrier_ready: std::sync::atomic::AtomicI32,
    barrier_target: std::sync::atomic::AtomicI32,
}

const SHM_FILE_DEFAULT: &str = "benchmark_results/tm_2pc_state.bin";

// ── Globals ─────────────────────────────────────────────
static mut G_STATE: *mut SharedState = std::ptr::null_mut();

// ── Lock helpers on shared state ────────────────────────
fn lock_shared() {
    let state = unsafe { &*G_STATE };
    loop {
        if state.lock_flag.load(Ordering::Relaxed) == 0
            && state
                .lock_flag
                .compare_exchange_weak(0, 1, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
        {
            return;
        }
        while state.lock_flag.load(Ordering::Relaxed) != 0 {
            std::hint::spin_loop();
        }
    }
}

fn unlock_shared() {
    let state = unsafe { &*G_STATE };
    state.lock_flag.store(0, Ordering::Release);
}

// ── Public API ──────────────────────────────────────────
pub fn tm_init() {
    let shm_file = std::env::var("TM_SHM_FILE").unwrap_or_else(|_| SHM_FILE_DEFAULT.to_string());
    let nproc: i32 = std::env::var("TM_NPROCESSES")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(1)
        .max(1);

    let page = page_size();
    let state_size = page; // one page for shared state
    let cpath = std::ffi::CString::new(shm_file.as_str()).unwrap();
    let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_CREAT, 0o644) };
    assert!(fd >= 0, "sgl-distributed: open({shm_file}) failed");

    if unsafe { libc::ftruncate(fd, state_size as i64) } < 0 {
        panic!("sgl-distributed: ftruncate({shm_file}, {state_size}) failed");
    }

    let base = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            state_size,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            fd,
            0,
        )
    };
    unsafe { libc::close(fd); }
    assert!(
        base != libc::MAP_FAILED,
        "sgl-distributed: mmap {shm_file} failed"
    );

    unsafe {
        G_STATE = base as *mut SharedState;
        let state = &*G_STATE;
        let expected = 0i32;
        if state
            .barrier_target
            .compare_exchange(expected, nproc, Ordering::AcqRel, Ordering::Relaxed)
            .is_ok()
        {
            state.barrier_ready.store(0, Ordering::Relaxed);
            state.lock_flag.store(0, Ordering::Relaxed);
            state.epoch.store(0, Ordering::Relaxed);
        }
    }

    // Each process initialises its own private TM region
    addrspace::tm_region_init();

    // Barrier: wait for all processes
    unsafe {
        let state = &*G_STATE;
        state.barrier_ready.fetch_add(1, Ordering::AcqRel);
        while state.barrier_ready.load(Ordering::Acquire)
            < state.barrier_target.load(Ordering::Acquire)
        {
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    eprintln!("[SGL-DISTRIBUTED] ready (nproc={nproc})");
}

pub fn tm_exit() {
    unsafe {
        if !G_STATE.is_null() {
            let state = &*G_STATE;
            state.barrier_ready.fetch_sub(1, Ordering::AcqRel);
        }
    }
}

pub fn tm_init_thread() {}
pub fn tm_exit_thread() {}

pub fn tm_begin() {
    lock_shared();
    fence(Ordering::SeqCst);
}

pub fn tm_commit() -> bool {
    fence(Ordering::SeqCst);
    unsafe {
        let state = &*G_STATE;
        state.epoch.fetch_add(1, Ordering::Release);
    }
    unlock_shared();
    true
}

pub fn tm_abort() {
    fence(Ordering::SeqCst);
    unlock_shared();
}

pub fn tm_abort_count() -> u64 { 0 }

// ── Read/write — direct memory access (lock provides isolation) ──

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

fn page_size() -> usize {
    let ps = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    if ps <= 0 { 4096 } else { ps as usize }
}
