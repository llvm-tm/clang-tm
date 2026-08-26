// ── TiKV Distributed TM Backend ──────────────────────────
// Demonstrates the expressive power of the TM abstraction:
// any distributed storage system can be wrapped with TM
// semantics.  TiKV provides transactional KV (Percolator-
// style 2PC), which maps directly to TM begin/read/write/
// commit.
//
// Architecture
// ────────────
//   Each TM address maps to a TiKV key: "tm:{region_offset:016x}"
//   At tm_begin(): start a TiKV optimistic transaction (snapshot).
//   At tm_read():   local write-set → TiKV get (lazy-fetch via snapshot).
//   At tm_write():  buffer in local write-set.
//   At tm_commit(): flush write-set to TiKV → 2PC commit.
//   At tm_abort():  rollback TiKV transaction.
//
// Environment
// ───────────
//   TM_TIKV_PD  - PD endpoint(s), comma-separated (default "127.0.0.1:2379")
//
// Prerequisites
// ─────────────
//   A running TiKV cluster (https://tikv.org/docs/latest/deploy/).
//   This backend uses tikv-client 0.4 from crates.io.
//
// Performance note
// ────────────────
//   Every TM read issues a gRPC call to TiKV (via block_on on Tokio).
//   This is extremely slow compared to shared-memory backends, but
//   demonstrates the correctness of the TM abstraction.

use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::OnceLock;

use tikv_client::{TransactionClient, Transaction};

pub use runtime_core::{Primitive, TmxAbort, TypedValue};

// ── Global state ─────────────────────────────────────────
// Tokio runtime + TiKV TransactionClient, created once at tm_init().
struct GlobalState {
    runtime: tokio::runtime::Runtime,
    client: TransactionClient,
}

static GLOBAL: OnceLock<GlobalState> = OnceLock::new();

fn global() -> &'static GlobalState {
    GLOBAL.get().expect("tm_init() not called")
}

// ── Per-thread transaction state ─────────────────────────
struct TxState {
    txn: Transaction,
    write_set: HashMap<Vec<u8>, Vec<u8>>,
    read_set: HashMap<Vec<u8>, Vec<u8>>,
}

thread_local! {
    static TX: RefCell<Option<TxState>> = const { RefCell::new(None) };
}

fn with_tx<F, R>(f: F) -> R
where
    F: FnOnce(&mut TxState) -> R,
{
    TX.with(|cell| {
        let mut borrow = cell.borrow_mut();
        let state = borrow.as_mut().expect("tm_begin() not called");
        f(state)
    })
}

// ── Key encoding ─────────────────────────────────────────
// Use offset from TM region start so different processes
// (with different virtual addresses) agree on the same key.
fn addr_key(addr: *const u8) -> Vec<u8> {
    let start = addrspace::tm_region_start();
    let offset = (addr as usize).wrapping_sub(start);
    format!("tm:{:016x}", offset).into_bytes()
}

// ── Public Rust API ──────────────────────────────────────
// Used by the Rust `tm` crate (transaction(), TmCell, etc.).
// These are Rust ABI — symbol names follow the backend
// convention (no prefix).

pub fn tm_init() {
    tikv_init();
}

pub fn tm_exit() {
    tikv_shutdown();
}

pub fn tm_init_thread() {}
pub fn tm_exit_thread() {}

pub fn tm_begin() {
    tikv_begin();
}

pub fn tm_commit() -> bool {
    tikv_commit()
}

pub fn tm_abort() {
    tikv_abort();
}

pub fn tm_abort_count() -> u64 { 0 }

// ── C FFI exports ────────────────────────────────────────
// Used by the C++ hook shim (tikv_backend.cpp).
// The `tikv_tm_` prefix avoids DATA/TEXT symbol conflicts:
// the TM hooks system defines DATA variables like `tm_begin`,
// `tm_end`, etc. as function pointers.  Rust's `extern "C"`
// functions are TEXT symbols — same name would conflict.
//
// To link: compile this crate as a staticlib:
//   [lib]
//   crate-type = ["lib", "staticlib"]
// then link libruntime_tikv.a with the C++ binary.

#[no_mangle]
pub extern "C" fn tikv_tm_init()        { tikv_init() }

#[no_mangle]
pub extern "C" fn tikv_tm_exit()        { tikv_shutdown() }

#[no_mangle]
pub extern "C" fn tikv_tm_begin()       { tikv_begin() }

#[no_mangle]
pub extern "C" fn tikv_tm_end() -> bool { tikv_commit() }

#[no_mangle]
pub extern "C" fn tikv_tm_read_u8(a: *const u8)    -> u8    { tm_read_u8(a as *mut u8) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_u16(a: *const u16)  -> u16   { tm_read_u16(a as *mut u16) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_u32(a: *const u32)  -> u32   { tm_read_u32(a as *mut u32) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_u64(a: *const u64)  -> u64   { tm_read_u64(a as *mut u64) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_i8(a: *const i8)    -> i8    { tm_read_i8(a as *mut i8) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_i16(a: *const i16)  -> i16   { tm_read_i16(a as *mut i16) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_i32(a: *const i32)  -> i32   { tm_read_i32(a as *mut i32) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_i64(a: *const i64)  -> i64   { tm_read_i64(a as *mut i64) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_f32(a: *const f32)  -> f32   { tm_read_f32(a as *mut f32) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_f64(a: *const f64)  -> f64   { tm_read_f64(a as *mut f64) }
#[no_mangle]
pub extern "C" fn tikv_tm_read_ptr(a: *const *const std::ffi::c_void) -> *mut std::ffi::c_void {
    tm_read_ptr(a as *mut *mut u8) as *mut std::ffi::c_void
}

#[no_mangle]
pub extern "C" fn tikv_tm_write_u8(a: *mut u8, v: u8)               { tm_write_u8(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_u16(a: *mut u16, v: u16)            { tm_write_u16(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_u32(a: *mut u32, v: u32)            { tm_write_u32(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_u64(a: *mut u64, v: u64)            { tm_write_u64(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_i8(a: *mut i8, v: i8)               { tm_write_i8(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_i16(a: *mut i16, v: i16)            { tm_write_i16(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_i32(a: *mut i32, v: i32)            { tm_write_i32(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_i64(a: *mut i64, v: i64)            { tm_write_i64(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_f32(a: *mut f32, v: f32)            { tm_write_f32(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_f64(a: *mut f64, v: f64)            { tm_write_f64(a, v) }
#[no_mangle]
pub extern "C" fn tikv_tm_write_ptr(a: *mut *mut std::ffi::c_void, v: *mut std::ffi::c_void) {
    tm_write_ptr(a as *mut *mut u8, v as *mut u8)
}

#[no_mangle]
pub extern "C" fn tikv_tm_get_thread_state() -> *mut std::ffi::c_void {
    std::ptr::null_mut()
}

// ── Internal implementation ──────────────────────────────
// Shared by both the Rust API and C FFI paths.

fn tikv_init() {
    let pd_str = std::env::var("TM_TIKV_PD")
        .unwrap_or_else(|_| "127.0.0.1:2379".to_string());
    let endpoints: Vec<&str> = pd_str.split(',').map(|s| s.trim()).collect();

    let runtime = tokio::runtime::Runtime::new()
        .expect("tikv: failed to create Tokio runtime");

    let client = runtime.block_on(async {
        TransactionClient::new(endpoints)
            .await
            .expect("tikv: failed to connect to TiKV cluster")
    });

    eprintln!(
        "[TIKV] connected to PD: {}  (region base: {:p})",
        pd_str,
        addrspace::tm_region_start() as *const u8,
    );

    if GLOBAL.set(GlobalState { runtime, client }).is_err() {
        panic!("tikv: tm_init called more than once");
    }

    runtime_core::tm_install_tmx_hook();
}

fn tikv_shutdown() {
    eprintln!("[TIKV] shutting down");
}

fn tikv_begin() {
    let g = global();
    let txn = g.runtime.block_on(async {
        g.client.begin_optimistic()
            .await
            .expect("tikv: begin_optimistic failed")
    });
    TX.with(|cell| {
        *cell.borrow_mut() = Some(TxState {
            txn,
            write_set: HashMap::new(),
            read_set: HashMap::new(),
        });
    });
}

fn tikv_commit() -> bool {
    let ok = with_tx(|state| {
        let g = global();
        for (key, val) in &state.write_set {
            g.runtime.block_on(async {
                state.txn
                    .put(key.clone(), val.clone())
                    .await
                    .expect("tikv: put during commit failed");
            });
        }
        let ok = g.runtime.block_on(async {
            state.txn.commit().await
        }).is_ok();
        ok
    });
    TX.with(|cell| *cell.borrow_mut() = None);
    ok
}

fn tikv_abort() {
    with_tx(|state| {
        let g = global();
        g.runtime.block_on(async {
            let _ = state.txn.rollback().await;
        });
    });
    TX.with(|cell| *cell.borrow_mut() = None);
}

// ── Read helpers ─────────────────────────────────────────
fn tikv_read(key: &[u8]) -> Option<Vec<u8>> {
    with_tx(|state| {
        if let Some(val) = state.write_set.get(key) {
            return Some(val.clone());
        }
        if let Some(val) = state.read_set.get(key) {
            return Some(val.clone());
        }
        let g = global();
        let val = g.runtime.block_on(async {
            state.txn.get(key.to_vec()).await
        });
        match val {
            Ok(Some(kv)) => {
                let bytes = kv.to_vec();
                state.read_set.insert(key.to_vec(), bytes.clone());
                Some(bytes)
            }
            Ok(None) => None,
            Err(_) => {
                // TiKV error (e.g. TxnNotFound from concurrent commit).
                // Abort the TiKV txn so it releases locks, then signal retry.
                let _ = g.runtime.block_on(async {
                    state.txn.rollback().await
                });
                std::panic::panic_any(TmxAbort);
            }
        }
    })
}

macro_rules! def_read {
    ($name:ident, $ty:ty) => {
        #[inline]
        pub fn $name(addr: *mut $ty) -> $ty {
            let key = addr_key(addr as *const u8);
            let val = tikv_read(&key);
            match val {
                Some(bytes) => {
                    let n = std::mem::size_of::<$ty>();
                    let copy_len = bytes.len().min(n);
                    let mut arr = [0u8; 8];
                    arr[..copy_len].copy_from_slice(&bytes[..copy_len]);
                    <$ty>::from_ne_bytes(arr[..n].try_into().unwrap())
                }
                None => unsafe { addr.read() },
            }
        }
    };
}

macro_rules! def_write {
    ($name:ident, $ty:ty) => {
        #[inline]
        pub fn $name(addr: *mut $ty, val: $ty) {
            let key = addr_key(addr as *const u8);
            let bytes: Vec<u8> = val.to_ne_bytes().to_vec();
            with_tx(|state| {
                state.write_set.insert(key, bytes);
            });
        }
    };
}

def_read!(tm_read_u8,   u8);
def_read!(tm_read_u16,  u16);
def_read!(tm_read_u32,  u32);
def_read!(tm_read_u64,  u64);
def_read!(tm_read_i8,   i8);
def_read!(tm_read_i16,  i16);
def_read!(tm_read_i32,  i32);
def_read!(tm_read_i64,  i64);
def_read!(tm_read_f32,  f32);
def_read!(tm_read_f64,  f64);

def_write!(tm_write_u8,   u8);
def_write!(tm_write_u16,  u16);
def_write!(tm_write_u32,  u32);
def_write!(tm_write_u64,  u64);
def_write!(tm_write_i8,   i8);
def_write!(tm_write_i16,  i16);
def_write!(tm_write_i32,  i32);
def_write!(tm_write_i64,  i64);
def_write!(tm_write_f32,  f32);
def_write!(tm_write_f64,  f64);

#[inline]
pub fn tm_read_ptr<T>(addr: *mut *mut T) -> *mut T {
    let key = addr_key(addr as *const u8);
    let val = tikv_read(&key);
    match val {
        Some(bytes) => {
            let n = std::mem::size_of::<usize>();
            let copy_len = bytes.len().min(n);
            let mut arr = [0u8; 8];
            arr[..copy_len].copy_from_slice(&bytes[..copy_len]);
            usize::from_ne_bytes(arr) as *mut T
        }
        None => unsafe { addr.read() },
    }
}

#[inline]
pub fn tm_write_ptr<T>(addr: *mut *mut T, val: *mut T) {
    let key = addr_key(addr as *const u8);
    let bytes: Vec<u8> = (val as usize).to_ne_bytes().to_vec();
    with_tx(|state| {
        state.write_set.insert(key, bytes);
    });
}

#[inline]
pub fn tm_read_raw(addr: *mut u8, dst: &mut [u8]) {
    let key = addr_key(addr);
    let val = tikv_read(&key);
    match val {
        Some(bytes) => {
            let n = dst.len().min(bytes.len());
            dst[..n].copy_from_slice(&bytes[..n]);
        }
        None => unsafe {
            std::ptr::copy_nonoverlapping(addr, dst.as_mut_ptr(), dst.len());
        },
    }
}

#[inline]
pub fn tm_write_raw(addr: *mut u8, src: &[u8]) {
    let key = addr_key(addr);
    with_tx(|state| {
        state.write_set.insert(key, src.to_vec());
    });
}
