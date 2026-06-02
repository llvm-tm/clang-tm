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
