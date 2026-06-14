// ── TmxAbort — panic payload for TM contention aborts ───
// Used by write-back backends (WBCTL, WBETL, NOrec, TL2, DUDETM)
// to unwind past the transaction body on contention, avoiding the
// "zombie window" where subsequent TM ops would run with aborted state.
// Caught by transaction()'s catch_unwind — only TM aborts are caught;
// real panics (user code) are re-panicked.
// The custom panic hook in tm_install_tmx_hook() filters out the
// "Box<dyn Any>" spam that Rust would otherwise print for every
// caught TmxAbort panic.
#[derive(Debug)]
pub struct TmxAbort;

/// Install a panic hook that suppresses TmxAbort panic messages.
/// TmxAbort panics are caught by transaction()'s catch_unwind and
/// are normal TM retries — the "Box<dyn Any>" output would be noise.
/// Call this once at tm_init() in any backend that uses TmxAbort.
pub fn tm_install_tmx_hook() {
    static HOOK_INSTALLED: std::sync::OnceLock<()> = std::sync::OnceLock::new();
    HOOK_INSTALLED.get_or_init(|| {
        let prev = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            if info.payload().downcast_ref::<TmxAbort>().is_some() { return; }
            prev(info);
        }));
    });
}

// ── WriteBack — deferred write for safe commit ──────────
// WriteBack::apply() encapsulates the unsafe ptr::write so that
// tm_commit() can be a safe function.  The safety contract is:
//   - The address was obtained from a TmCell (valid, aligned, live)
//   - commit() holds exclusive access (locks acquired, read-set validated)
#[derive(Clone, Debug)]
pub enum WriteBack {
    U8(usize, u8),
    U16(usize, u16),
    U32(usize, u32),
    U64(usize, u64),
    Bytes(usize, Box<[u8]>),
}

impl WriteBack {
    pub fn addr(&self) -> usize {
        match *self {
            WriteBack::U8(a, _) | WriteBack::U16(a, _)
                | WriteBack::U32(a, _) | WriteBack::U64(a, _)
                | WriteBack::Bytes(a, _) => a,
        }
    }

    /// Apply this write-back to memory.
    ///
    /// # Safety contract (caller must ensure)
    ///
    /// 1. `addr` points to valid, aligned memory of the correct size.
    /// 2. No other thread aliases `addr` during this call (exclusive access
    ///    is guaranteed by the TM commit protocol — locks held, read-set
    ///    validated, global commit lock acquired).
    pub fn apply(self) {
        unsafe {
            match self {
                WriteBack::U8(addr, v) => (addr as *mut u8).write(v),
                WriteBack::U16(addr, v) => (addr as *mut u16).write(v),
                WriteBack::U32(addr, v) => (addr as *mut u32).write(v),
                WriteBack::U64(addr, v) => (addr as *mut u64).write(v),
                WriteBack::Bytes(addr, b) => {
                    std::ptr::copy_nonoverlapping(b.as_ptr(), addr as *mut u8, b.len());
                }
            }
        }
    }
}

// ── TypedValue — type-safe write-set entry ──────────────
#[derive(Clone, Debug)]
pub enum TypedValue {
    U8(u8),
    U16(u16),
    U32(u32),
    U64(u64),
    Bytes(Box<[u8]>),
}

impl TypedValue {
    pub fn as_u64(&self) -> u64 {
        match *self {
            TypedValue::U8(v) => v as u64,
            TypedValue::U16(v) => v as u64,
            TypedValue::U32(v) => v as u64,
            TypedValue::U64(v) => v,
            TypedValue::Bytes(_) => 0,
        }
    }

    pub fn byte_size(&self) -> usize {
        match *self {
            TypedValue::U8(_) => 1,
            TypedValue::U16(_) => 2,
            TypedValue::U32(_) => 4,
            TypedValue::U64(_) => 8,
            TypedValue::Bytes(ref b) => b.len(),
        }
    }

    pub fn into_write_back(self, addr: usize) -> WriteBack {
        match self {
            TypedValue::U8(v) => WriteBack::U8(addr, v),
            TypedValue::U16(v) => WriteBack::U16(addr, v),
            TypedValue::U32(v) => WriteBack::U32(addr, v),
            TypedValue::U64(v) => WriteBack::U64(addr, v),
            TypedValue::Bytes(b) => WriteBack::Bytes(addr, b),
        }
    }
}

// ── Primitive trait — all TM-trackable types ────────────
pub trait Primitive: Copy + 'static {
    fn to_typed(self) -> TypedValue;
    fn from_typed(v: &TypedValue) -> Self;
}

impl Primitive for u8 {
    fn to_typed(self) -> TypedValue { TypedValue::U8(self) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U8(x) => x, _ => unreachable!() } }
}
impl Primitive for u16 {
    fn to_typed(self) -> TypedValue { TypedValue::U16(self) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U16(x) => x, _ => unreachable!() } }
}
impl Primitive for u32 {
    fn to_typed(self) -> TypedValue { TypedValue::U32(self) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U32(x) => x, _ => unreachable!() } }
}
impl Primitive for u64 {
    fn to_typed(self) -> TypedValue { TypedValue::U64(self) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U64(x) => x, _ => unreachable!() } }
}
impl Primitive for i32 {
    fn to_typed(self) -> TypedValue { TypedValue::U32(self as u32) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U32(x) => x as i32, _ => unreachable!() } }
}
impl Primitive for i64 {
    fn to_typed(self) -> TypedValue { TypedValue::U64(self as u64) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U64(x) => x as i64, _ => unreachable!() } }
}
impl Primitive for i16 {
    fn to_typed(self) -> TypedValue { TypedValue::U16(self as u16) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U16(x) => x as i16, _ => unreachable!() } }
}
impl Primitive for i8 {
    fn to_typed(self) -> TypedValue { TypedValue::U8(self as u8) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U8(x) => x as i8, _ => unreachable!() } }
}
impl Primitive for f32 {
    fn to_typed(self) -> TypedValue { TypedValue::U32(self.to_bits()) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U32(x) => f32::from_bits(x), _ => unreachable!() } }
}
impl Primitive for f64 {
    fn to_typed(self) -> TypedValue { TypedValue::U64(self.to_bits()) }
    fn from_typed(v: &TypedValue) -> Self { match *v { TypedValue::U64(x) => f64::from_bits(x), _ => unreachable!() } }
}

/// Trait for TM-trackable types of any size.
///
/// Automatically implemented for all `Primitive` types (≤ 8 bytes, fits in
/// the word-level write-set).  User-defined types can `impl TmRaw` manually
/// to enable TM reads/writes for their arbitrary-size structs via byte buffers.
pub trait TmRaw: Sized {
    fn to_bytes(&self) -> Box<[u8]>;
    fn from_bytes(buf: &[u8]) -> Self;
}

macro_rules! impl_raw_for_primitive {
    ($ty:ty) => {
        impl TmRaw for $ty {
            fn to_bytes(&self) -> Box<[u8]> {
                let raw = self.to_typed().as_u64();
                let n = core::mem::size_of::<$ty>();
                let mut buf = vec![0u8; n];
                buf.copy_from_slice(&raw.to_ne_bytes()[..n]);
                buf.into_boxed_slice()
            }
            fn from_bytes(buf: &[u8]) -> Self {
                let mut raw = [0u8; 8];
                raw[..buf.len()].copy_from_slice(buf);
                let v = u64::from_ne_bytes(raw);
                Self::from_typed(&TypedValue::U64(v))
            }
        }
    };
}

impl_raw_for_primitive!(u8);
impl_raw_for_primitive!(u16);
impl_raw_for_primitive!(u32);
impl_raw_for_primitive!(u64);
impl_raw_for_primitive!(i8);
impl_raw_for_primitive!(i16);
impl_raw_for_primitive!(i32);
impl_raw_for_primitive!(i64);
impl_raw_for_primitive!(f32);
impl_raw_for_primitive!(f64);

/// Re-export the TM address-space check from the addrspace crate.
pub use addrspace::is_tm_address;

// ── Simulation mode (deterministic replay) ────────────────
//
// When the "simulation" feature is enabled, backends replace their
// thread_local! TxState storage with a HashMap<SimThreadId, TxState>
// so the simulator can multiplex simulated threads on real OS threads.
//
// The simulator sets SIM_THREAD_ID before each backend call and uses
// SimStateStore::snapshot()/restore() for checkpointing.

/// Opaque identifier for a simulated thread.
#[cfg(feature = "simulation")]
pub type SimThreadId = u64;

#[cfg(feature = "simulation")]
thread_local! {
    static SIM_THREAD_ID: std::cell::Cell<Option<SimThreadId>> =
        const { std::cell::Cell::new(None) };
}

/// Set the simulated thread ID for the current OS thread.
/// Must be called before any backend function when in simulation mode.
#[cfg(feature = "simulation")]
pub fn set_sim_thread_id(id: SimThreadId) {
    SIM_THREAD_ID.with(|c| c.set(Some(id)));
}

/// Clear the simulated thread ID.
#[cfg(feature = "simulation")]
pub fn clear_sim_thread_id() {
    SIM_THREAD_ID.with(|c| c.set(None));
}

/// Get the current simulated thread ID (panics if not set).
#[cfg(feature = "simulation")]
pub fn current_sim_thread_id() -> SimThreadId {
    SIM_THREAD_ID.with(|c| c.get().expect("sim_thread_id not set"))
}

/// Generic per-thread state storage for simulation mode.
///
/// Replaces `thread_local! { static TX: RefCell<Option<T>> }` in backends.
/// The simulator maps each simulated thread to a real OS thread via
/// `set_sim_thread_id()`. This store retrieves the correct state by ID.
#[cfg(feature = "simulation")]
pub struct SimStateStore<T> {
    inner: std::sync::Mutex<std::collections::HashMap<SimThreadId, T>>,
}

#[cfg(feature = "simulation")]
impl<T> SimStateStore<T> {
    /// Create a new empty store.
    pub fn new() -> Self {
        SimStateStore {
            inner: std::sync::Mutex::new(std::collections::HashMap::new()),
        }
    }

    /// Access the state of the currently active simulated thread.
    pub fn with<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&mut T) -> R,
    {
        let tid = current_sim_thread_id();
        let mut map = self.inner.lock().unwrap();
        let state = map.get_mut(&tid).expect("no state for sim thread");
        f(state)
    }

    /// Access the state of a specific simulated thread.
    pub fn with_tid<F, R>(&self, tid: SimThreadId, f: F) -> R
    where
        F: FnOnce(&mut T) -> R,
    {
        let mut map = self.inner.lock().unwrap();
        let state = map.get_mut(&tid).expect("no state for sim thread");
        f(state)
    }

    /// Insert state for a simulated thread (called on thread init).
    pub fn insert(&self, tid: SimThreadId, state: T) {
        let mut map = self.inner.lock().unwrap();
        map.insert(tid, state);
    }

    /// Remove and return state for a simulated thread (called on thread exit).
    pub fn remove(&self, tid: SimThreadId) -> Option<T> {
        let mut map = self.inner.lock().unwrap();
        map.remove(&tid)
    }

    /// Snapshot all thread states (for checkpointing).
    pub fn snapshot(&self) -> std::collections::HashMap<SimThreadId, T>
    where
        T: Clone,
    {
        let map = self.inner.lock().unwrap();
        map.clone()
    }

    /// Restore thread states from a snapshot.
    pub fn restore(&self, map: std::collections::HashMap<SimThreadId, T>) {
        let mut inner = self.inner.lock().unwrap();
        *inner = map;
    }
}
