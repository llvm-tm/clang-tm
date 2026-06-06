// ── Distributed Bank Test ──────────────────────────────────
// Tests sgl-distributed backend: multiple processes share bank
// accounts and coordinate via the distributed SGL lock.
//
// The test forks child processes. Each process performs random
// transfers inside transactions. After all children finish, the
// parent verifies the total balance is conserved.
//
// Usage:
//   cargo run --release --no-default-features \
//     --features sgl-distributed --bin test_distrib_bank

use std::ffi::CString;
use std::path::Path;

const ACCOUNT_FILE: &str = "test_distrib_accounts.bin";
const FILE_SIZE: i64 = 64 * 1024 * 1024;
const DATA_OFFSET: usize = 4096;
const NUM_ACCOUNTS: usize = 10;
const INITIAL_BALANCE: i64 = 1000;
const EXPECTED_TOTAL: i64 = (NUM_ACCOUNTS as i64) * INITIAL_BALANCE;
const TRANSFERS_PER_PROC: usize = 50;
const MAX_AMOUNT: i64 = 100;

struct ShmRegion {
    ptr: *mut u8,
    size: usize,
}

impl ShmRegion {
    fn new(path: &str, size: usize) -> Self {
        if let Some(parent) = Path::new(path).parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let cpath = CString::new(path).unwrap();
        let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_CREAT, 0o644) };
        assert!(fd >= 0, "open({}) failed", path);
        let ret = unsafe { libc::ftruncate(fd, size as i64) };
        assert!(ret >= 0, "ftruncate failed");
        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };
        unsafe { libc::close(fd); }
        assert!(ptr != libc::MAP_FAILED, "mmap({}) failed", path);
        unsafe { std::ptr::write_bytes(ptr, 0, size); }
        unsafe { libc::msync(ptr, size, libc::MS_SYNC); }
        ShmRegion { ptr: ptr as *mut u8, size }
    }

    fn accounts(&self) -> *mut i64 {
        unsafe { self.ptr.add(DATA_OFFSET) as *mut i64 }
    }
}

impl Drop for ShmRegion {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.ptr as *mut libc::c_void, self.size); }
        let _ = std::fs::remove_file(ACCOUNT_FILE);
    }
}

fn do_transfers(accounts: *mut i64, count: usize) {
    for _ in 0..count {
        tm::transaction(|_tx| unsafe {
            let src = fastrand::usize(0..NUM_ACCOUNTS);
            let dst = fastrand::usize(0..NUM_ACCOUNTS);
            if src == dst { return; }
            let amount = fastrand::i64(1..=MAX_AMOUNT);
            let sb = accounts.add(1 + src).read();
            if sb < amount { return; }
            accounts.add(1 + src).write(sb - amount);
            accounts.add(1 + dst).write(accounts.add(1 + dst).read() + amount);
        });
    }
}

fn main() {
    tm::tm_init();

    let shm = ShmRegion::new(ACCOUNT_FILE, FILE_SIZE as usize);
    let accounts = shm.accounts();

    // Initialise accounts (only first process should do this; we're
    // single-process before fork, so we init once).
    tm::transaction(|_tx| unsafe {
        for i in 0..NUM_ACCOUNTS {
            accounts.add(1 + i).write(INITIAL_BALANCE);
        }
    });
    unsafe { libc::msync(shm.ptr as *mut libc::c_void, shm.size, libc::MS_SYNC); }

    // Fork child processes
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        panic!("fork failed");
    }

    if pid == 0 {
        // ── Child process ──
        do_transfers(accounts, TRANSFERS_PER_PROC);
        tm::tm_exit();
        eprintln!("CHILD: {} transfers done", TRANSFERS_PER_PROC);
        std::process::exit(0);
    } else {
        // ── Parent process ──
        do_transfers(accounts, TRANSFERS_PER_PROC);
        eprintln!("PARENT: {} transfers done", TRANSFERS_PER_PROC);

        // Wait for child
        let mut status: i32 = 0;
        unsafe { libc::waitpid(pid, &mut status as *mut i32, 0); }

        // Verify total is conserved
        tm::transaction(|_tx| {
            let mut total: i64 = 0;
            for i in 0..NUM_ACCOUNTS {
                let bal = unsafe { accounts.add(1 + i).read() };
                assert!(bal >= 0, "account {} has negative balance {}", i, bal);
                total += bal;
            }
            assert_eq!(total, EXPECTED_TOTAL,
                "total mismatch: got {}, expected {}", total, EXPECTED_TOTAL);
            eprintln!("PASS: total={}, conserved across {} processes", total, 2);
        });
    }

    tm::tm_exit();
}
