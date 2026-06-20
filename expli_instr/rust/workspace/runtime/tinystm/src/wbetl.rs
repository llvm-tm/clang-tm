use crate::common::*;
use core::sync::atomic::{compiler_fence, fence, Ordering};

fn read_word<T: Primitive>(addr: usize) -> T {
    compiler_fence(Ordering::SeqCst);
    if !tx_active() { return unsafe { (addr as *const T).read() }; }
    if let Some(entry) = with_tx(|tx| ws_get(&tx.write_set, addr).map(|e| e.value.clone())) {
        return T::from_typed(&entry);
    }
    loop {
        {
            let mut rspins = 0u64;
            while is_locked(addr) {
                if with_tx(|tx| tx.locked_addrs.contains(&lock_index(addr))) { break; }
                #[cfg(feature = "simulation")]
                { with_tx(|tx| tx.aborted = true); return unsafe { (addr as *const T).read() }; }
                rspins += 1;
                if rspins > 5000 { with_tx(|tx| tx.aborted = true); return unsafe { (addr as *const T).read() }; }
                std::hint::spin_loop();
            }
        }
        let version = read_version(addr);
        let value: T = unsafe { (addr as *const T).read() };
        if read_version(addr) != version { continue; }
        let retry = with_tx(|tx| {
            if version > tx.end_version {
                if tx.snapshot_extend() { return true; }
                tx.aborted = true;
            } else {
                tx.read_set.push((addr, version));
            }
            false
        });
        if retry { continue; }
        return value;
    }
}

fn write_word<T: Primitive>(addr: usize, val: T) {
    compiler_fence(Ordering::SeqCst);
    if !tx_active() { unsafe { (addr as *mut T).write(val); } return; }
    if with_tx(|tx| tx.aborted) { return; }
    let tv = val.to_typed();
    // Existing write-set entry → update in-place, no lock needed
    if with_tx(|tx| ws_contains(&tx.write_set, addr)) {
        with_tx(|tx| {
            ws_write(&mut tx.write_set, addr, tv.clone());
            tx.write_backs.push(tv.clone().into_write_back(addr));
        });
        return;
    }
    // Wait for lock with bounded spin (prevents deadlock)
    {
        let mut spins = 0u64;
        while is_locked(addr) {
            if with_tx(|tx| tx.locked_addrs.contains(&lock_index(addr))) { break; }
            #[cfg(feature = "simulation")]
            { with_tx(|tx| tx.aborted = true); return; }
            spins += 1;
            if spins > 5000 { with_tx(|tx| tx.aborted = true); return; }
            std::hint::spin_loop();
        }
    }
    let version = read_version(addr);
    if version > with_tx(|tx| tx.start_version) { with_tx(|tx| tx.aborted = true); return; }
    let lock_idx = lock_index(addr);
    // Self-ownership check: different addresses may hash to the same lock
    if with_tx(|tx| tx.locked_addrs.contains(&lock_idx)) {
        with_tx(|tx| {
            ws_write(&mut tx.write_set, addr, tv.clone());
            tx.write_backs.push(tv.into_write_back(addr));
            tx.read_set.push((addr, version));
        });
        return;
    }
    let mut lock_spins = 0u64;
    while !try_lock_at_index(lock_idx) {
        #[cfg(feature = "simulation")]
        { with_tx(|tx| tx.aborted = true); return; }
        lock_spins += 1;
        if lock_spins > 10000
            || (is_locked(addr) && read_version(addr) > with_tx(|tx| tx.start_version))
        {
            with_tx(|tx| tx.aborted = true); return;
        }
        std::hint::spin_loop();
    }
    with_tx(|tx| { tx.locked_addrs.push(lock_idx);
        ws_write(&mut tx.write_set, addr, tv.clone());
        tx.write_backs.push(tv.into_write_back(addr));
        tx.read_set.push((addr, version));
    });
}

fn read_raw_bytes(addr: usize, dst: &mut [u8]) {
    for (i, byte) in dst.iter_mut().enumerate() { *byte = read_word::<u8>(addr + i); }
}

fn write_raw_bytes(addr: usize, src: &[u8]) {
    compiler_fence(Ordering::SeqCst);
    if !tx_active() { unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len()); } return; }
    if with_tx(|tx| tx.aborted) { return; }
    let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
    {
        let mut spins = 0u64;
        while is_locked(addr) {
            if with_tx(|tx| tx.locked_addrs.contains(&lock_index(addr))) { break; }
            #[cfg(feature = "simulation")]
            { with_tx(|tx| tx.aborted = true); return; }
            spins += 1;
            if spins > 5000 { with_tx(|tx| tx.aborted = true); return; }
            std::hint::spin_loop();
        }
    }
    let version = read_version(addr);
    if version > with_tx(|tx| tx.start_version) { with_tx(|tx| tx.aborted = true); return; }
    let lock_idx = lock_index(addr);
    if with_tx(|tx| tx.locked_addrs.contains(&lock_idx)) {
        with_tx(|tx| {
            ws_write(&mut tx.write_set, addr, tv.clone());
            tx.write_backs.push(tv.clone().into_write_back(addr));
        });
        return;
    }
    let mut lock_spins = 0u64;
    while !try_lock_at_index(lock_idx) {
        #[cfg(feature = "simulation")]
        { with_tx(|tx| tx.aborted = true); return; }
        lock_spins += 1;
        if lock_spins > 10000
            || (is_locked(addr) && read_version(addr) > with_tx(|tx| tx.start_version))
        {
            with_tx(|tx| tx.aborted = true); return;
        }
        std::hint::spin_loop();
    }
    with_tx(|tx| { tx.locked_addrs.push(lock_idx);
        ws_write(&mut tx.write_set, addr, tv.clone());
        tx.write_backs.push(tv.into_write_back(addr));
    });
}

pub fn tm_abort() {
    with_tx(|tx| {
        if !tx.locked_addrs.is_empty() {
            unlock_indices(&tx.locked_addrs);
        }
    });
    flush_tx();
}

pub fn tm_commit() -> bool {
    let tx = match flush_tx() { Some(t) => t, None => return true };
    fence(Ordering::SeqCst);
    if tx.aborted {
        if !tx.locked_addrs.is_empty() { unlock_indices(&tx.locked_addrs); }
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        #[cfg(feature = "stats")] crate::common::TM_STATS.aborts.fetch_add(1, Ordering::Relaxed);
        return false;
    }
    if tx.write_set.is_empty() { return true; }
    gc_tick();
    fence(Ordering::SeqCst);
    if !validate_read_set(&tx.read_set) { unlock_indices(&tx.locked_addrs); TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed); #[cfg(feature = "stats")] crate::common::TM_STATS.aborts.fetch_add(1, Ordering::Relaxed); return false; }
    for wb in tx.write_backs { wb.apply(); }
    fence(Ordering::SeqCst);
    unlock_indices(&tx.locked_addrs);
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
