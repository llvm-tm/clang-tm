// ── MemoryAccess trait — compile-time switch between TM and plain access ──
// Lets a single data structure implementation work both inside and outside
// transactions without code duplication.
//
// Use a const generic (`bool USE_TM`) to select the policy at compile time:
//
//   fn lookup<K: Ord, V>(tree: &RBTree<K, V>, key: K) -> Option<&RBNode<K, V>>
//       where K: Copy
//   {
//       let mut n = if USE_TM { TmAccess::read(&tree.root) } else { UntrackedAccess::read(&tree.root) };
//       while let Some(node) = n {
//           let nk = TmAccess::read(&node.key);  // or UntrackedAccess
//           if key == nk { return n; }
//           n = /* ... */;
//       }
//       None
//   }
//
// Or, with helper dispatch:
//
//   macro_rules! load { ($ptr:expr) => { MemoryAccess::<USE_TM>::load($ptr) }; }
//   macro_rules! store { ($ptr:expr, $val:expr) => { MemoryAccess::<USE_TM>::store($ptr, $val) }; }

use std::ptr;

/// Trait for memory access policies.
pub trait MemAccess {
    unsafe fn load<T>(addr: *const T) -> T;
    unsafe fn store<T>(addr: *mut T, val: T);
}

/// TM-tracked access (delegates to tm_read_*/tm_write_*).
pub struct TmAccess;

impl MemAccess for TmAccess {
    #[inline]
    unsafe fn load<T>(addr: *const T) -> T {
        // Dispatch by size and pointer-ness.
        // In production, call the appropriate tm_read_i*/tm_read_ptr.
        extern "C" {
            fn tm_read_i1(addr: *const u8) -> u8;
            fn tm_read_i2(addr: *const u16) -> u16;
            fn tm_read_i4(addr: *const u32) -> u32;
            fn tm_read_i8(addr: *const u64) -> u64;
            fn tm_read_ptr(addr: *const *mut u8) -> *mut u8;
        }
        let size = core::mem::size_of::<T>();
        // Use a type-size dispatch since Rust doesn't have if constexpr
        // for const generics in expressions (yet).
        // This casts through raw bytes — safe because tm_read returns the
        // correct bit pattern for the requested size.
        if core::mem::size_of::<*const u8>() == size && core::mem::align_of::<T>() >= core::mem::align_of::<*const u8>() {
            // Pointer-sized: use tm_read_ptr
            let result = tm_read_ptr(addr as *const *mut u8);
            ptr::read(&result as *const *mut u8 as *const T)
        } else {
            match size {
                1 => {
                    let r = tm_read_i1(addr as *const u8);
                    ptr::read(&r as *const u8 as *const T)
                }
                2 => {
                    let r = tm_read_i2(addr as *const u16);
                    ptr::read(&r as *const u16 as *const T)
                }
                4 => {
                    let r = tm_read_i4(addr as *const u32);
                    ptr::read(&r as *const u32 as *const T)
                }
                8 => {
                    let r = tm_read_i8(addr as *const u64);
                    ptr::read(&r as *const u64 as *const T)
                }
                _ => panic!("MemoryAccess::load: unsupported type size {}", size),
            }
        }
    }

    #[inline]
    unsafe fn store<T>(addr: *mut T, val: T) {
        extern "C" {
            fn tm_write_i1(addr: *mut u8, val: u8);
            fn tm_write_i2(addr: *mut u16, val: u16);
            fn tm_write_i4(addr: *mut u32, val: u32);
            fn tm_write_i8(addr: *mut u64, val: i64);
            fn tm_write_ptr(addr: *mut *mut u8, val: *mut u8);
        }
        let size = core::mem::size_of::<T>();
        if core::mem::size_of::<*const u8>() == size && core::mem::align_of::<T>() >= core::mem::align_of::<*const u8>() {
            tm_write_ptr(addr as *mut *mut u8, ptr::read(&val as *const T as *const *mut u8));
        } else {
            match size {
                1 => tm_write_i1(addr as *mut u8, ptr::read(&val as *const T as *const u8)),
                2 => tm_write_i2(addr as *mut u16, ptr::read(&val as *const T as *const u16)),
                4 => tm_write_i4(addr as *mut u32, ptr::read(&val as *const T as *const u32)),
                8 => tm_write_i8(addr as *mut u64, ptr::read(&val as *const T as *const i64)),
                _ => panic!("MemoryAccess::store: unsupported type size {}", size),
            }
        }
    }
}

/// Untracked (plain) access — zero-overhead direct dereference.
pub struct UntrackedAccess;

impl MemAccess for UntrackedAccess {
    #[inline]
    unsafe fn load<T>(addr: *const T) -> T {
        ptr::read(addr)
    }

    #[inline]
    unsafe fn store<T>(addr: *mut T, val: T) {
        ptr::write(addr, val);
    }
}

// ── Helper: dispatch on a const bool ──
// Use select_access!(USE_TM)::load(&node.key)
// (Currently unused; kept as a reference pattern.)
// macro_rules! select_access { ... }

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn untracked_load_store_roundtrip() {
        unsafe {
            let mut x: i64 = 42;
            let v = UntrackedAccess::load(&x);
            assert_eq!(v, 42);
            UntrackedAccess::store(&mut x, 99);
            assert_eq!(x, 99);
        }
    }
}
