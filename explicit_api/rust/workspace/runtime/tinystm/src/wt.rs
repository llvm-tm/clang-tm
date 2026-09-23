use crate::common::*;
use core::sync::atomic::{compiler_fence, fence, Ordering};

fn tx_aborted() -> bool {
    tx_active() && with_tx(|tx| tx.aborted)
}

fn read_word<T: Primitive>(addr: usize) -> T {
    compiler_fence(Ordering::SeqCst);
    if !runtime_core::is_tm_address(addr as *const u8) {
        return unsafe { (addr as *const T).read() };
    }
    if !tx_active() {
        return unsafe { (addr as *const T).read() };
    }
    if let Some(entry) = with_tx(|tx| ws_get(&tx.write_set, addr).map(|e| e.value.clone())) {
        return T::from_typed(&entry);
    }
    loop {
        {
            let mut rspins = 0u64;
            while is_locked(addr) {
                if with_tx(|tx| tx.locked_addrs.contains(&lock_index(addr))) {
                    break;
                }
                #[cfg(feature = "simulation")]
                {
                    with_tx(|tx| tx.aborted = true);
                    return unsafe { (addr as *const T).read() };
                }
                rspins += 1;
                if rspins > 5000 {
                    with_tx(|tx| tx.aborted = true);
                    return unsafe { (addr as *const T).read() };
                }
                std::hint::spin_loop();
            }
        }
        let version = read_version(addr);
        let value: T = unsafe { (addr as *const T).read() };
        if read_version(addr) != version {
            continue;
        }
        let retry = with_tx(|tx| {
            if version > tx.end_version {
                if tx.snapshot_extend() {
                    return true;
                }
                tx.aborted = true;
            } else {
                tx.read_set.push((addr, version));
            }
            false
        });
        if retry {
            continue;
        }
        return value;
    }
}

fn write_word<T: Primitive>(addr: usize, val: T) {
    compiler_fence(Ordering::SeqCst);
    if !runtime_core::is_tm_address(addr as *const u8) {
        unsafe {
            (addr as *mut T).write(val);
        }
        return;
    }
    if tx_aborted() {
        return;
    }
    if !tx_active() {
        unsafe {
            (addr as *mut T).write(val);
        }
        return;
    }
    let tv = val.to_typed();
    // Existing write-set entry → update in-place, no lock needed
    if with_tx(|tx| ws_contains(&tx.write_set, addr)) {
        let old_val: T = unsafe { (addr as *const T).read() };
        unsafe {
            (addr as *mut T).write(val);
        }
        with_tx(|tx| {
            ws_write(&mut tx.write_set, addr, tv.clone());
            tx.undo_backs.push(old_val.to_typed().into_write_back(addr));
        });
        return;
    }
    // Wait for lock with bounded spin (prevents deadlock from circular wait)
    {
        let mut spins = 0u64;
        while is_locked(addr) {
            // In simulation mode, the lock-holder will never release.
            #[cfg(feature = "simulation")]
            {
                with_tx(|tx| tx.aborted = true);
                return;
            }
            spins += 1;
            if spins > 5000 {
                with_tx(|tx| tx.aborted = true);
                return;
            }
            std::hint::spin_loop();
        }
    }
    let version = read_version(addr);
    if version > with_tx(|tx| tx.start_version) {
        with_tx(|tx| tx.aborted = true);
        return;
    }
    let lock_idx = lock_index(addr);
    // Self-ownership check: different addresses may hash to the same lock
    if with_tx(|tx| tx.locked_addrs.contains(&lock_idx)) {
        let old_val: T = unsafe { (addr as *const T).read() };
        unsafe {
            (addr as *mut T).write(val);
        }
        let old_tv = old_val.to_typed();
        with_tx(|tx| {
            tx.undo_backs.push(old_tv.into_write_back(addr));
            ws_write(&mut tx.write_set, addr, tv);
            tx.read_set.push((addr, version));
        });
        return;
    }
    let mut lock_spins = 0u64;
    while !try_lock_at_index(lock_idx) {
        // In simulation mode, the lock-holder will never release.
        #[cfg(feature = "simulation")]
        {
            with_tx(|tx| tx.aborted = true);
            return;
        }
        lock_spins += 1;
        if lock_spins > 10000
            || (is_locked(addr) && read_version(addr) > with_tx(|tx| tx.start_version))
        {
            with_tx(|tx| tx.aborted = true);
            return;
        }
        std::hint::spin_loop();
    }
    let old_val: T = unsafe { (addr as *const T).read() };
    let old_ver = version_at_index(lock_idx);
    unsafe {
        (addr as *mut T).write(val);
    }
    let old_tv = old_val.to_typed();
    with_tx(|tx| {
        tx.locked_addrs.push(lock_idx);
        tx.locked_old_versions.push((lock_idx, old_ver));
        tx.undo_backs.push(old_tv.into_write_back(addr));
        ws_write(&mut tx.write_set, addr, tv);
        tx.read_set.push((addr, version));
    });
}

fn read_raw_bytes(addr: usize, dst: &mut [u8]) {
    for (i, byte) in dst.iter_mut().enumerate() {
        *byte = read_word::<u8>(addr + i);
    }
}

fn write_raw_bytes(addr: usize, src: &[u8]) {
    compiler_fence(Ordering::SeqCst);
    if !runtime_core::is_tm_address(addr as *const u8) {
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len());
        }
        return;
    }
    if tx_aborted() {
        return;
    }
    if !tx_active() {
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len());
        }
        return;
    }
    // Existing write-set entry → update in-place, no lock needed
    if with_tx(|tx| ws_contains(&tx.write_set, addr)) {
        let mut old_buf = vec![0u8; src.len()];
        unsafe {
            std::ptr::copy_nonoverlapping(addr as *const u8, old_buf.as_mut_ptr(), src.len());
        }
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len());
        }
        let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
        let old_tv = TypedValue::Bytes(old_buf.into_boxed_slice());
        with_tx(|tx| {
            ws_write(&mut tx.write_set, addr, tv);
            tx.undo_backs.push(old_tv.into_write_back(addr));
        });
        return;
    }
    {
        let mut spins = 0u64;
        while is_locked(addr) {
            #[cfg(feature = "simulation")]
            {
                with_tx(|tx| tx.aborted = true);
                return;
            }
            spins += 1;
            if spins > 5000 {
                with_tx(|tx| tx.aborted = true);
                return;
            }
            std::hint::spin_loop();
        }
    }
    let version = read_version(addr);
    if version > with_tx(|tx| tx.start_version) {
        with_tx(|tx| tx.aborted = true);
        return;
    }
    let lock_idx = lock_index(addr);
    // Self-ownership check: different addresses may hash to the same lock
    if with_tx(|tx| tx.locked_addrs.contains(&lock_idx)) {
        let mut old_buf = vec![0u8; src.len()];
        unsafe {
            std::ptr::copy_nonoverlapping(addr as *const u8, old_buf.as_mut_ptr(), src.len());
        }
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len());
        }
        let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
        let old_tv = TypedValue::Bytes(old_buf.into_boxed_slice());
        with_tx(|tx| {
            tx.undo_backs.push(old_tv.into_write_back(addr));
            ws_write(&mut tx.write_set, addr, tv);
        });
        return;
    }
    let mut lock_spins = 0u64;
    while !try_lock_at_index(lock_idx) {
        #[cfg(feature = "simulation")]
        {
            with_tx(|tx| tx.aborted = true);
            return;
        }
        lock_spins += 1;
        if lock_spins > 10000
            || (is_locked(addr) && read_version(addr) > with_tx(|tx| tx.start_version))
        {
            with_tx(|tx| tx.aborted = true);
            return;
        }
        std::hint::spin_loop();
    }
    let mut old_buf = vec![0u8; src.len()];
    let old_ver = version_at_index(lock_idx);
    unsafe {
        std::ptr::copy_nonoverlapping(addr as *const u8, old_buf.as_mut_ptr(), src.len());
    }
    unsafe {
        std::ptr::copy_nonoverlapping(src.as_ptr(), addr as *mut u8, src.len());
    }
    let tv = TypedValue::Bytes(src.to_vec().into_boxed_slice());
    let old_tv = TypedValue::Bytes(old_buf.into_boxed_slice());
    with_tx(|tx| {
        tx.locked_addrs.push(lock_idx);
        tx.locked_old_versions.push((lock_idx, old_ver));
        tx.undo_backs.push(old_tv.into_write_back(addr));
        ws_write(&mut tx.write_set, addr, tv);
    });
}

pub fn tm_abort() {
    if let Some(tx) = flush_tx() {
        // Reverse order: repeated writes to one address form a chain
        // (pre -> v1 -> v2); undo entries must be applied newest-first so
        // the oldest (pre-transaction) value wins (review-05 R-01).
        for u in tx.undo_backs.into_iter().rev() {
            u.apply();
        }
        if !tx.locked_addrs.is_empty() {
            unlock_indices_restore(&tx.locked_old_versions);
        }
    }
}

pub fn tm_commit() -> bool {
    let tx = match flush_tx() {
        Some(t) => t,
        None => return true,
    };
    fence(Ordering::SeqCst);
    if tx.aborted {
        for u in tx.undo_backs.into_iter().rev() {
            u.apply();
        }
        unlock_indices_restore(&tx.locked_old_versions);
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        #[cfg(feature = "stats")]
        crate::common::TM_STATS
            .aborts
            .fetch_add(1, Ordering::Relaxed);
        return false;
    }
    if tx.write_set.is_empty() {
        return true;
    }
    let ts = gc_tick();
    fence(Ordering::SeqCst);
    if !validate_read_set(&tx.read_set) {
        for u in tx.undo_backs.into_iter().rev() {
            u.apply();
        }
        unlock_indices_restore(&tx.locked_old_versions);
        TM_ABORT_COUNT.fetch_add(1, Ordering::Relaxed);
        #[cfg(feature = "stats")]
        crate::common::TM_STATS
            .aborts
            .fetch_add(1, Ordering::Relaxed);
        return false;
    }
    unlock_indices_stamp(&tx.locked_addrs, ts);
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

#[inline]
pub fn tm_read_ptr<T>(a: *mut *mut T) -> *mut T {
    read_word::<u64>(a as usize) as *mut T
}
#[inline]
pub fn tm_write_ptr<T>(a: *mut *mut T, v: *mut T) {
    write_word::<u64>(a as usize, v as u64);
}
#[inline]
pub fn tm_read_raw(a: *mut u8, d: &mut [u8]) {
    read_raw_bytes(a as usize, d);
}
#[inline]
pub fn tm_write_raw(a: *mut u8, s: &[u8]) {
    write_raw_bytes(a as usize, s);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    // WT keeps a process-global lock table; serialize tests so two cells that
    // hash to the same lock index cannot make a test spin-abort (flaky).
    static TM_LOCK: Mutex<()> = Mutex::new(());

    fn init_and_alloc() -> *mut u64 {
        crate::tm_init();
        let p = addrspace::tm_region_malloc(core::mem::size_of::<u64>()) as *mut u64;
        assert!(!p.is_null());
        unsafe { p.write(0) };
        p
    }

    // R-01: undo_backs must be applied in reverse; repeated writes to one
    // address form a value chain (pre -> v1 -> v2) and the pre-transaction
    // value must survive abort.
    #[test]
    fn repeated_write_then_abort_restores_pre_txn_value() {
        let _g = TM_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let a = init_and_alloc();
        crate::tm_begin();
        crate::tm_write_u64(a, 1);
        crate::tm_write_u64(a, 2);
        with_tx(|tx| tx.aborted = true);
        assert!(!crate::tm_commit());
        unsafe {
            assert_eq!(a.read(), 0, "abort must restore the pre-transaction value");
        }
    }

    #[test]
    fn repeated_write_then_abort_then_new_txn_sees_pre_txn_value() {
        let _g = TM_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let a = init_and_alloc();
        crate::tm_begin();
        crate::tm_write_u64(a, 10);
        crate::tm_write_u64(a, 20);
        crate::tm_write_u64(a, 30);
        with_tx(|tx| tx.aborted = true);
        assert!(!crate::tm_commit());
        crate::tm_begin();
        let seen = crate::tm_read_u64(a);
        assert!(crate::tm_commit());
        assert_eq!(
            seen, 0,
            "fresh transaction must observe the pre-abort value"
        );
    }

    #[test]
    fn repeated_write_then_commit_keeps_last_value() {
        let _g = TM_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let a = init_and_alloc();
        crate::tm_begin();
        crate::tm_write_u64(a, 1);
        crate::tm_write_u64(a, 2);
        crate::tm_write_u64(a, 3);
        assert!(crate::tm_commit());
        unsafe {
            assert_eq!(a.read(), 3);
        }
    }

    #[test]
    fn repeated_raw_write_then_abort_restores_pre_txn_bytes() {
        let _g = TM_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        crate::tm_init();
        let p = addrspace::tm_region_malloc(8) as *mut u8;
        assert!(!p.is_null());
        unsafe { std::ptr::write_bytes(p, 0, 8) };
        crate::tm_begin();
        crate::tm_write_raw(p, b"AAAAAAA");
        crate::tm_write_raw(p, b"BBBBBBB");
        with_tx(|tx| tx.aborted = true);
        assert!(!crate::tm_commit());
        let mut buf = [0u8; 7];
        unsafe {
            std::ptr::copy_nonoverlapping(p as *const u8, buf.as_mut_ptr(), 7);
        }
        assert_eq!(
            &buf, b"\0\0\0\0\0\0\0",
            "abort must restore the raw pre-transaction bytes"
        );
    }
}
