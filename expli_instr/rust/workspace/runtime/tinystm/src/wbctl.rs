use crate::common::*;
use core::sync::atomic::{fence, Ordering};

fn read_word<T: Primitive>(addr: usize) -> T {
    fence(Ordering::SeqCst);
    if !tx_active() { return unsafe { (addr as *const T).read() }; }
    if let Some(entry) = with_tx(|tx| tx.write_set.get(&addr).map(|e| e.value.clone())) {
        return T::from_typed(&entry);
    }
    loop {
        while is_locked(addr) { std::hint::spin_loop(); }
        let version = read_version(addr);
        let value: T = unsafe { (addr as *const T).read() };
        if read_version(addr) != version { continue; }
        if with_tx(|tx| {
            if version > tx.start_version { true }
            else { tx.read_set.push((addr, version)); false }
        }) { std::panic::panic_any(TmxAbort); }
        return value;
    }
}

fn write_word<T: Primitive>(addr: usize, val: T) {
    fence(Ordering::SeqCst);
    if !tx_active() { unsafe { (addr as *mut T).write(val); } return; }
    let tv = val.to_typed();
    with_tx(|tx| {
        use std::collections::hash_map::Entry;
        while is_locked(addr) { std::hint::spin_loop(); }
        let version = read_version(addr);
        if version > tx.start_version { std::panic::panic_any(TmxAbort); }
        let entry = tx.write_set.entry(addr);
        entry.or_insert(WriteEntry { value: tv });
        tx.read_set.push((addr, version));
    });
}

fn read_raw_bytes(addr: usize, dst: &mut [u8]) {
    for (i, byte) in dst.iter_mut().enumerate() { *byte = read_word::<u8>(addr + i); }
}

fn write_raw_bytes(addr: usize, src: &[u8]) {
    fence(Ordering::SeqCst);
    if !tx_active() { unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len()); } return; }
    let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
    with_tx(|tx| {
        tx.write_set.entry(addr).and_modify(|e| e.value = tv.clone()).or_insert(WriteEntry { value: tv });
    });
}

fn apply_typed_value(addr: usize, tv: &TypedValue) {
    unsafe {
        match tv {
            TypedValue::U8(v) => (addr as *mut u8).write(*v),
            TypedValue::U16(v) => (addr as *mut u16).write(*v),
            TypedValue::U32(v) => (addr as *mut u32).write(*v),
            TypedValue::U64(v) => (addr as *mut u64).write(*v),
            TypedValue::Bytes(b) => {
                std::ptr::copy_nonoverlapping(b.as_ptr(), addr as *mut u8, b.len());
            }
        }
    }
}

pub fn tm_abort() {
    flush_tx();
}

pub fn tm_commit() -> bool {
    let tx = match flush_tx() { Some(t) => t, None => return true };
    fence(Ordering::SeqCst);
    if tx.write_set.is_empty() { update_read_write_stats(tx.read_set.len(), 0); return true; }
    let addrs: Vec<usize> = tx.write_set.keys().copied().collect();
    gc_acquire(); fence(Ordering::SeqCst);
    let idxs = lock_write_addrs(&addrs);
    if !validate_read_set(&tx.read_set) { unlock_indices(&idxs); gc_release_and_inc(); TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed); #[cfg(feature = "stats")] crate::common::TM_STATS.aborts.fetch_add(1, Ordering::Relaxed); return false; }
    for (addr, entry) in &tx.write_set { apply_typed_value(*addr, &entry.value); }
    fence(Ordering::SeqCst);
    unlock_indices(&idxs);
    gc_release_and_inc();
    update_read_write_stats(tx.read_set.len(), tx.write_set.len());
    true
}

crate::def_read!(tm_read_u8, u8);
crate::def_read!(tm_read_u16, u16);
crate::def_read!(tm_read_u32, u32);
crate::def_read!(tm_read_u64, u64);
crate::def_read!(tm_read_i32, i32);
crate::def_read!(tm_read_i16, i16);
crate::def_read!(tm_read_i8, i8);
crate::def_read!(tm_read_i64, i64);
crate::def_read!(tm_read_f32, f32);
crate::def_read!(tm_read_f64, f64);

crate::def_write!(tm_write_u8, u8);
crate::def_write!(tm_write_u16, u16);
crate::def_write!(tm_write_u32, u32);
crate::def_write!(tm_write_u64, u64);
crate::def_write!(tm_write_i32, i32);
crate::def_write!(tm_write_i16, i16);
crate::def_write!(tm_write_i8, i8);
crate::def_write!(tm_write_i64, i64);
crate::def_write!(tm_write_f32, f32);
crate::def_write!(tm_write_f64, f64);

#[inline] pub fn tm_read_ptr<T>(a: *mut *mut T) -> *mut T { read_word::<u64>(a as usize) as *mut T }
#[inline] pub fn tm_write_ptr<T>(a: *mut *mut T, v: *mut T) { write_word::<u64>(a as usize, v as u64); }
#[inline] pub fn tm_read_raw(a: *mut u8, d: &mut [u8]) { read_raw_bytes(a as usize, d); }
#[inline] pub fn tm_write_raw(a: *mut u8, s: &[u8]) { write_raw_bytes(a as usize, s); }
