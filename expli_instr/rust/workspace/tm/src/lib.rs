/// Safe public TM API.
use runtime_core::TmxAbort;

// ── Backend selection via feature flags ─────────────────
// The `wbctl` (default), `wbetl`, `wt`, `norec`, `tl2`,
// `swisstm`, and `dudetm` features select which backend to use.
// Only one may be active at a time.

#[cfg(feature = "norec")]
pub use runtime_norec::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "tl2")]
pub use runtime_tl2::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "swisstm")]
pub use runtime_swisstm::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "dudetm")]
pub use runtime_dudetm::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "tsxsgl")]
pub use runtime_tsxsgl::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "nvhtm")]
pub use runtime_nvhtm::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "spht")]
pub use runtime_spht::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "leftright")]
pub use runtime_leftright::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "romulus")]
pub use runtime_romulus::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "leftright-single")]
pub use runtime_leftright_single::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "xtm")]
pub use runtime_xtm::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "sgl-persistent")]
pub use runtime_sgl_persistent::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(feature = "sgl-distributed")]
pub use runtime_sgl_distributed::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(any(feature = "wbctl", feature = "wbetl", feature = "wt"))]
pub use runtime_tinystm::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count, tm_abort,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

// ── Feature exclusivity ─────────────────────────────────
// Ensure exactly one backend is selected (at most one, at least one).
macro_rules! exclusive_backend {
    ($me:expr, $($others:expr),+) => {
        #[cfg(all(feature = $me, any($(feature = $others),+)))]
        compile_error!(concat!(
            "Multiple backends enabled: '", $me,
            "' conflicts with another backend. Enable exactly one."
        ));
    };
}

exclusive_backend!("norec",             "tl2", "swisstm", "dudetm", "tsxsgl", "nvhtm", "spht", "leftright", "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("tl2",               "swisstm", "dudetm", "tsxsgl", "nvhtm", "spht", "leftright", "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("swisstm",           "dudetm", "tsxsgl", "nvhtm", "spht", "leftright", "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("dudetm",            "tsxsgl", "nvhtm", "spht", "leftright", "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("tsxsgl",            "nvhtm", "spht", "leftright", "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("nvhtm",             "spht", "leftright", "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("spht",              "leftright", "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("leftright",         "leftright-single", "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("leftright-single",  "romulus", "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("romulus",           "xtm", "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("xtm",               "sgl-persistent", "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("sgl-persistent",    "sgl-distributed", "wbctl", "wbetl", "wt");
exclusive_backend!("sgl-distributed",   "wbctl", "wbetl", "wt");
exclusive_backend!("wbctl",             "wbetl", "wt");
exclusive_backend!("wbetl",             "wt");

#[cfg(not(any(
    feature = "wbctl", feature = "wbetl", feature = "wt",
    feature = "norec", feature = "tl2", feature = "swisstm", feature = "dudetm",
    feature = "tsxsgl", feature = "nvhtm", feature = "spht",
    feature = "leftright", feature = "leftright_single",
    feature = "romulus", feature = "xtm",
    feature = "sgl_persistent", feature = "sgl_distributed",
)))]
compile_error!(
    "At least one backend feature must be enabled: wbctl, wbetl, wt, norec, tl2, swisstm, dudetm, tsxsgl, nvhtm, spht, leftright, leftright_single, romulus, xtm, sgl_persistent, sgl_distributed"
);

// ── TmPrimitive trait ──────────────────────────────────
pub use runtime_core::TmRaw;

pub trait TmPrimitive: Copy + Send + Sync + 'static {
    unsafe fn tm_read(ptr: *mut Self) -> Self;
    unsafe fn tm_write(ptr: *mut Self, val: Self);
}

macro_rules! impl_primitive {
    ($ty:ty, $read:ident, $write:ident) => {
        impl TmPrimitive for $ty {
            unsafe fn tm_read(ptr: *mut Self) -> Self {
                $read(ptr as *mut _) as Self
            }
            unsafe fn tm_write(ptr: *mut Self, val: Self) {
                $write(ptr as *mut _, val as _)
            }
        }
    };
}

impl_primitive!(u8,  tm_read_u8,  tm_write_u8);
impl_primitive!(i8,  tm_read_i8,  tm_write_i8);
impl_primitive!(u16, tm_read_u16, tm_write_u16);
impl_primitive!(i16, tm_read_i16, tm_write_i16);
impl_primitive!(u32, tm_read_u32, tm_write_u32);
impl_primitive!(i32, tm_read_i32, tm_write_i32);
impl_primitive!(u64, tm_read_u64, tm_write_u64);
impl_primitive!(i64, tm_read_i64, tm_write_i64);
impl_primitive!(f32, tm_read_f32, tm_write_f32);
impl_primitive!(f64, tm_read_f64, tm_write_f64);

// ── TmPtr<T> — pointer wrapper for TM-tracked pointers ──
pub struct TmPtr<T> { ptr: *mut T }

unsafe impl<T: Send> Send for TmPtr<T> {}
unsafe impl<T: Send> Sync for TmPtr<T> {}

impl<T> Clone for TmPtr<T> { fn clone(&self) -> Self { TmPtr { ptr: self.ptr } } }
impl<T> Copy for TmPtr<T> {}

impl<T> TmPtr<T> {
    pub fn new(ptr: *mut T) -> Self { TmPtr { ptr } }
    pub fn null() -> Self { TmPtr { ptr: std::ptr::null_mut() } }
    pub fn get(self) -> *mut T { self.ptr }
}

impl<T: Send + Sync + 'static> TmPrimitive for TmPtr<T> {
    unsafe fn tm_read(ptr: *mut Self) -> Self {
        let raw = tm_read_ptr(ptr as *mut *mut u8);
        TmPtr { ptr: raw as *mut T }
    }
    unsafe fn tm_write(ptr: *mut Self, val: Self) {
        tm_write_ptr(ptr as *mut *mut u8, val.ptr as *mut u8);
    }
}

// ── TmCell ─────────────────────────────────────────────
/// A TM-tracked cell whose value lives in the TM address-space region.
///
/// `TmCell::new(val)` allocates space in the 16 GB mmap'ed TM region
/// (via `addrspace::tm_region_malloc`) and writes `val` there.
/// Inside a transaction, `tx.read(&cell)` / `tx.write(&cell, …)`
/// use the TM backend's protocol to ensure atomicity and isolation.
///
/// The backing TM-region allocation is **not** freed on `drop` —
/// it persists until process exit, matching the C++ `expli::TM<T>`
/// convention.

/// Unconditionally Send + Sync wrapper around `*mut T`.
/// SAFETY: TmCell guarantees that all access goes through the TM
/// protocol, so raw pointer synchronization is always correct.
#[repr(transparent)]
struct TmCellPtr<T>(*mut T);
unsafe impl<T> Send for TmCellPtr<T> {}
unsafe impl<T> Sync for TmCellPtr<T> {}

pub struct TmCell<T: TmPrimitive> {
    ptr: TmCellPtr<T>,
}

impl<T: TmPrimitive> TmCell<T> {
    pub fn new(val: T) -> Self {
        let raw_ptr = addrspace::tm_region_malloc(size_of::<T>()) as *mut T;
        unsafe { raw_ptr.write(val); }
        TmCell { ptr: TmCellPtr(raw_ptr) }
    }

    pub fn ptr(&self) -> *mut T { self.ptr.0 }
}

impl TmCell<u8> {
    /// Generic byte-buffer read for arbitrary types stored in a `TmCell<u8>`.
    /// The cell must be large enough to hold `dst.len()` bytes.
    pub fn read_raw(&self, dst: &mut [u8]) {
        tm_read_raw(self.ptr.0, dst);
    }

    /// Generic byte-buffer write for arbitrary types stored in a `TmCell<u8>`.
    pub fn write_raw(&self, src: &[u8]) {
        tm_write_raw(self.ptr.0, src);
    }
}

// ── Transaction ─────────────────────────────────────────
pub struct Transaction { _private: () }

impl Transaction {
    pub fn read<T: TmPrimitive>(&self, cell: &TmCell<T>) -> T {
        unsafe { T::tm_read(cell.ptr.0) }
    }

    pub fn write<T: TmPrimitive>(&self, cell: &TmCell<T>, val: T) {
        unsafe { T::tm_write(cell.ptr.0, val) };
    }

    /// Read arbitrary bytes from a `TmCell<u8>` acting as a byte buffer.
    pub fn read_raw(&self, cell: &TmCell<u8>, dst: &mut [u8]) {
        tm_read_raw(cell.ptr.0, dst);
    }

    /// Write arbitrary bytes to a `TmCell<u8>` acting as a byte buffer.
    pub fn write_raw(&self, cell: &TmCell<u8>, src: &[u8]) {
        tm_write_raw(cell.ptr.0, src);
    }
}

// ── transaction() ───────────────────────────────────────
pub fn transaction<T, F>(f: F) -> T
where
    F: Fn(&Transaction) -> T,
{
    loop {
        tm_begin();
        let tx = Transaction { _private: () };
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(&tx)));
        let committed = match result {
            Ok(_) => tm_commit(),
            Err(payload) => {
                // Only TM aborts (TmxAbort) are caught — retry automatically.
                // Real user panics are re-panicked after cleanup.
                tm_abort();
                if payload.downcast_ref::<TmxAbort>().is_some() {
                    continue; // TM contention — retry
                }
                std::panic::resume_unwind(payload); // real panic — propagate
            }
        };
        if let Ok(val) = result {
            if committed { return val; }
        }
    }
}
