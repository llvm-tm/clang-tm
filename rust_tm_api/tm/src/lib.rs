/// Safe public TM API.

// ── Backend selection via feature flags ─────────────────
// The `wbctl` (default), `wbetl`, `wt`, `norec`, `tl2`,
// `swisstm`, and `dudetm` features select which backend to use.
// Only one may be active at a time.

#[cfg(feature = "norec")]
pub use runtime_norec::{
    tm_init, tm_exit, tm_init_thread, tm_exit_thread,
    tm_begin, tm_commit, tm_abort_count,
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
    tm_begin, tm_commit, tm_abort_count,
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
    tm_begin, tm_commit, tm_abort_count,
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
    tm_begin, tm_commit, tm_abort_count,
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
    tm_begin, tm_commit, tm_abort_count,
    tm_read_u8, tm_read_u16, tm_read_u32, tm_read_u64,
    tm_read_i8, tm_read_i16, tm_read_i32, tm_read_i64,
    tm_read_f32, tm_read_f64, tm_read_ptr,
    tm_write_u8, tm_write_u16, tm_write_u32, tm_write_u64,
    tm_write_i8, tm_write_i16, tm_write_i32, tm_write_i64,
    tm_write_f32, tm_write_f64, tm_write_ptr,
    tm_read_raw, tm_write_raw,
};

#[cfg(not(any(
    feature = "wbctl", feature = "wbetl", feature = "wt",
    feature = "norec", feature = "tl2", feature = "swisstm", feature = "dudetm"
)))]
compile_error!(
    "At least one backend feature must be enabled: wbctl, wbetl, wt, norec, tl2, swisstm, dudetm"
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
pub struct TmCell<T: TmPrimitive> {
    value: core::cell::UnsafeCell<T>,
}

unsafe impl<T: TmPrimitive> Sync for TmCell<T> {}

impl<T: TmPrimitive> TmCell<T> {
    pub const fn new(val: T) -> Self {
        TmCell { value: core::cell::UnsafeCell::new(val) }
    }

    pub fn ptr(&self) -> *mut T { self.value.get() }
}

impl TmCell<u8> {
    /// Generic byte-buffer read for arbitrary types stored in a `TmCell<u8>`.
    /// The cell must be large enough to hold `dst.len()` bytes.
    pub fn read_raw(&self, dst: &mut [u8]) {
        tm_read_raw(self.value.get(), dst);
    }

    /// Generic byte-buffer write for arbitrary types stored in a `TmCell<u8>`.
    pub fn write_raw(&self, src: &[u8]) {
        tm_write_raw(self.value.get(), src);
    }
}

// ── Transaction ─────────────────────────────────────────
pub struct Transaction { _private: () }

impl Transaction {
    pub fn read<T: TmPrimitive>(&self, cell: &TmCell<T>) -> T {
        unsafe { T::tm_read(cell.value.get()) }
    }

    pub fn write<T: TmPrimitive>(&self, cell: &TmCell<T>, val: T) {
        unsafe { T::tm_write(cell.value.get(), val) };
    }

    /// Read arbitrary bytes from a `TmCell<u8>` acting as a byte buffer.
    pub fn read_raw(&self, cell: &TmCell<u8>, dst: &mut [u8]) {
        tm_read_raw(cell.value.get(), dst);
    }

    /// Write arbitrary bytes to a `TmCell<u8>` acting as a byte buffer.
    pub fn write_raw(&self, cell: &TmCell<u8>, src: &[u8]) {
        tm_write_raw(cell.value.get(), src);
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
        let committed = tm_commit();
        match result {
            Ok(val) => { if committed { return val; } }
            Err(_) => { continue; }
        }
    }
}
